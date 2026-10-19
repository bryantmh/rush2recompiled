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
//
// A mod's rush2/new_cars/<ID>.glb and <ID>.json add a car instead: the same container, as one of the mod car types
// 57-72 (rush2::car2049::set_mod_car), with the physics, sounds and options of the Rush 2 car the .json names as its
// base and the wheels where it puts them.

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <set>
#include <filesystem>
#include <initializer_list>
#include <iterator>
#include <cctype>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "json/json.hpp"
#include "librecomp/mods.hpp"
#include "recomp.h"

#include "asset_catalog.h"
#include "assets.h"
#include "car2049.h"
#include "car_mods.h"
#include "data_files.h"
#include "players4.h"
#include "paint_colors.h"
#include "texture_derive.h"
#include "texture_upscale.h"
#include "track2049_convert.h"
#include "track_cache.h"

namespace {
    using rush2::upscale::Image;

    constexpr int types = rush2::car2049::rush2_types;
    constexpr int first_car_asset = 0x1D;
    constexpr uint32_t car_slot_size = 0x8001CF28; // largest car asset: every car slot is allocated at it
    constexpr int stripe_asset = 0x1C; // holds CARPALETTE
    constexpr uint32_t game_state = 0x8010C0D0;
    constexpr uint32_t race_player_count = 0x8010C3E2; // s16: human players
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
    constexpr uint32_t cache_version = 5;

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
        int scheme = 0;              // a new car's color scheme (its .json's "schemes", each a glb) the part is of
        bool hood = false;           // of the hood camera's hood (H0_L / H2_L), not the body
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
    std::mutex palette_mutex;
    bool palette_loaded = false;

    // From the ROM (no game memory), once: a background plan (rush2::car_mods::prepare) needs it before the boot.
    bool load_car_palette() {
        std::lock_guard lock{ palette_mutex };
        if (palette_loaded) return true;
        std::vector<uint8_t> d;
        if (!rush2::assets::rom_original(stripe_asset, d) || d.size() < 0x28) return false;
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
            palette_loaded = true;
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
    // vertices per G_VTX. With several color schemes, each scheme's triangles are a block between two scheme markers
    // (rush2::car2049::scheme_marker), which a car's copy turns into a branch past the block for the schemes it doesn't
    // show. Returns the list's offset.
    // hood: the hood camera's hood's parts instead of the body's.
    uint32_t emit_body(std::vector<uint8_t>& d, const Model& model, const std::vector<StandIn>& stand_ins,
                       const std::vector<uint32_t>& load_lists, float& radius, bool hood = false) {
        std::vector<uint8_t> vertices;
        std::vector<std::pair<uint32_t, uint32_t>> list; // commands; a G_VTX's w1 is relative to the vertices
        std::vector<size_t> vtx_commands;
        auto cmd = [&](uint32_t w0, uint32_t w1) { list.emplace_back(w0, w1); };
        // Render mode and combiner as Rush 2's car panels set them (PICKUPD0_TOP1).
        cmd(0xE200001C, 0xC8112230);
        cmd(0xFC1216AC, 0xF0FFFE38);
        const float light[3] = { 0.30f, 0.80f, 0.52f };
        int schemes = 1;
        for (const Part& part : model.parts) schemes = std::max(schemes, part.scheme + 1);
        for (int scheme = 0; scheme < schemes; scheme++) {
            if (schemes > 1) cmd(0, rush2::car2049::scheme_marker(scheme, false));
            for (size_t si = 0; si < stand_ins.size(); si++) {
                const StandIn& s = stand_ins[si];
                bool loaded = false;
                for (const Part& part : model.parts) {
                    if (part.image != s.image || part.mask != s.mask || part.scheme != scheme || part.hood != hood) continue;
                    if (!loaded) {
                        cmd(0xDE000000, load_lists[si]);
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
            if (schemes > 1) cmd(0, rush2::car2049::scheme_marker(scheme, true));
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
        // The hood camera's hood, as a converted 2049 car's (its HOOD): H0_L, and H2_L too (Rush 2 swaps it in by panel
        // 0's damage; the body's hood folds instead), each its own list. H0_R / H2_R stay empty.
        bool has_hood = std::any_of(model.parts.begin(), model.parts.end(), [](const Part& p) { return p.hood; });
        float hood_radius = 0.0f;
        uint32_t hood0 = has_hood ? emit_body(d, model, stand_ins, load_lists, hood_radius, true) : 0;
        uint32_t hood2 = has_hood ? emit_body(d, model, stand_ins, load_lists, hood_radius, true) : 0;
        align8(d);
        uint32_t name_table = uint32_t(d.size());
        for (size_t i = 0; i < names.size(); i++) {
            bool body = names[i] == prefix + "FRAME1";
            uint32_t hood = names[i] == prefix + "H0_L" ? hood0 : names[i] == prefix + "H2_L" ? hood2 : 0;
            uint32_t r = model_table + uint32_t(i) * 0x34;
            put32(d, r, body ? 2 : 1);
            for (int l = 0; l < 4; l++) {
                uint32_t e = r + 4 + l * 12;
                float dist = l == 0 ? 80.0f : (body && l == 1) ? 600.0f : 0.0f;
                uint32_t list = l == 0 ? (body ? lod0 : hood ? hood : empty) : (body && l == 1) ? lod1 : 0;
                uint32_t bits;
                memcpy(&bits, &dist, 4);
                put32(d, e + 4, bits);
                put32(d, e + 8, list);
            }
            std::vector<uint8_t> n(0x18, 0);
            memcpy(n.data(), names[i].data(), std::min<size_t>(15, names[i].size()));
            float rad = body ? radius / 16.0f : hood ? hood_radius / 16.0f : 0.5f;
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
        // A livery's: the copy a damaged panel draws (car2049's add_damage_textures), and its key.
        std::vector<uint8_t> scuffed;
        uint64_t scuffed_key = 0;
    };

    // A livery stand-in's damaged copy, as make_stand_in scuffs a car's: its colors lighter and darker in blotches of
    // 3 x 3 texels (rush2::derive scuffs the full-size image the same way, paint_mod). Never the same as the texels, so
    // every texture of a livery has its own damaged load.
    void scuff_true_color(TrueColor& s, uint32_t seed) {
        auto hash = [seed](int x, int y) {
            uint32_t h = uint32_t(x) * 73856093u ^ uint32_t(y) * 19349663u ^ seed * 83492791u;
            h ^= h >> 13;
            h *= 0x5BD1E995u;
            h ^= h >> 15;
            return float(h & 0xFFFF) / 65535.0f;
        };
        s.scuffed = s.texels;
        for (size_t i = 0; i < size_t(s.width) * s.height; i++) {
            uint16_t c = uint16_t((s.texels[i * 2] << 8) | s.texels[i * 2 + 1]);
            float k = 1.0f + (hash(int(i % s.width) / 3, int(i / s.width) / 3) - 0.5f) * 0.45f;
            uint16_t out = c & 1;
            for (int shift : { 11, 6, 1 }) {
                int v = std::clamp(int(std::lround(((c >> shift) & 31) * k)), 0, 31);
                out |= uint16_t(v << shift);
            }
            s.scuffed[i * 2] = uint8_t(out >> 8);
            s.scuffed[i * 2 + 1] = uint8_t(out);
        }
        if (s.scuffed == s.texels && !s.scuffed.empty()) s.scuffed[1] ^= 2; // a black image: one blue step
        s.scuffed_key = rush2::upscale::rgba16_key(s.scuffed, s.width, s.height);
    }
    struct ModelMod {
        std::string mod_id, file;
        uint64_t glb_hash = 0;
        std::shared_ptr<const Model> model;
        std::vector<TrueColor> stand_ins;
        size_t triangles = 0;
        std::shared_ptr<ModelMod> hood; // a livery's hood camera hood (its .json's "hoods")
    };
    std::map<std::string, ModelMod> model_mods; // by model name (file name, upper case)
    std::string model_signature;

    std::string upper(std::string s) {
        for (char& c : s) c = char(std::toupper((unsigned char)c));
        return s;
    }

    // livery: a livery's (16 x 16, 512 bytes: a car carries every livery in each of its copies).
    void make_true_color(const Model& model, TrueColor& s, bool livery = false) {
        Image image;
        if (s.image < 0 || !rush2::upscale::decode_image(model.images[s.image], image) || image.empty()) {
            image.width = image.height = 1;
            image.rgba = { 200, 200, 200, 255 };
        }
        float aspect = float(image.width) / float(image.height);
        s.width = livery ? 16 : aspect >= 1.5f ? 64 : 32; // 4 KB at most: TMEM
        s.height = livery ? 16 : aspect <= 0.67f ? 64 : 32;
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

    // A mod model's stand-ins, kept on disk by the glb (decoding the images is the slow part). small: a livery's.
    bool load_model_mod(const std::vector<uint8_t>& glb, ModelMod& m, std::string& error, bool small = false) {
        auto model = std::make_shared<Model>();
        if (!parse_glb(glb, *model, error)) return false;
        for (const Part& p : model->parts) m.triangles += p.tris.size() / 3;
        m.glb_hash = rush2::track_cache::hash(glb.data(), glb.size(), cache_version);
        char name[40];
        snprintf(name, sizeof(name), small ? "livery_%016llx" : "modelmod_%016llx", (unsigned long long)m.glb_hash);
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
                make_true_color(*model, s, small);
                for (uint32_t v : { uint32_t(s.image), s.width, s.height }) add32(blob, v);
                blob.insert(blob.end(), s.texels.begin(), s.texels.end());
                m.stand_ins.push_back(std::move(s));
            }
            rush2::track_cache::save_blob(name, m.glb_hash, blob);
        }
        if (small) {
            for (TrueColor& s : m.stand_ins) scuff_true_color(s, uint32_t(m.glb_hash) * 977u + uint32_t(s.image));
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
    // A true-color stand-in's texture load (its texels at `texels`), as the commands of a list.
    std::vector<std::pair<uint32_t, uint32_t>> true_color_load(const TrueColor& s, uint32_t texels) {
        uint32_t line = s.width * 2 / 8;
        uint32_t masks = 0, maskt = 0;
        while ((1u << masks) < s.width) masks++;
        while ((1u << maskt) < s.height) maskt++;
        uint32_t tile = (maskt << 14) | (masks << 4);
        return { { 0xFD100000u, texels }, { 0xF5100000u, 0x07000000u | tile }, { 0xE6000000u, 0u },
                 { 0xF3000000u, 0x07000000u | ((s.width * s.height - 1) << 12) | (2048 / line) },
                 { 0xE7000000u, 0u }, { 0xF5100000u | (line << 9), tile },
                 { 0xF2000000u, (((s.width - 1) * 4) << 12) | ((s.height - 1) * 4) } };
    }

    // loads: each stand-in's texture-load list, called (G_DL) in place of loading it in the list (a livery's, whose
    // loads car2049's add_damage_textures inlines per panel).
    uint32_t emit_model(std::vector<uint8_t>& d, const ModelMod& mm, const std::vector<uint32_t>& texels,
                        const ListState& st, bool lit, float& radius, const std::vector<uint32_t>* loads = nullptr) {
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
                    if (loads != nullptr) cmd(0xDE000000, (*loads)[si]);
                    else for (auto [w0, w1] : true_color_load(s, texels[si])) cmd(w0, w1);
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

    // Rebuilds the models of container `base` that a model mod replaces (into out).
    bool patch_models(int index, const std::vector<uint8_t>& base, std::vector<uint8_t>& out) {
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
        return true;
    }

    bool append_liveries(int index, std::vector<uint8_t>& out);

    // rush2::assets::Patcher: model mods' models, then a car's liveries.
    bool patch_container(uint8_t* rdram, int index, const std::vector<uint8_t>& base, std::vector<uint8_t>& out) {
        bool changed = patch_models(index, base, out);
        if (!changed) out = base;
        changed = append_liveries(index, out) || changed;
        if (!changed) return false;
        // A car's asset copies live in slots of a fixed size (func_800A37F4): make them hold it.
        bool car = (index >= first_car_asset && index < first_car_asset + types) || (index >= 0x71 && index <= 0x7D);
        if (car && out.size() > (uint32_t)MEM_W(0, (int32_t)car_slot_size)) {
            MEM_W(0, (int32_t)car_slot_size) = (int32_t)((out.size() + 15) & ~15u);
        }
        fflush(stdout);
        return true;
    }

    // ---- liveries --------------------------------------------------------------------------------------------------
    // A mod's rush2/liveries/<ID>.json ({"car": "BRUISER", "styles": ["A.glb", ...]}, the glbs beside it) gives a car
    // more looks (docs/car_mods.md#liveries). Each livery is a whole mesh in the car's space, drawn as authored with
    // true-color stand-ins (16 x 16; rush2::derive draws the mod's images), appended to the car's container as a body
    // of its own, as a one-body car's (a new car's): livery n is "~Lnn"FRAME1, with the car's FRAME1 LODs (unlit or lit
    // lists, by each LOD's flag 0x10: a Rush 2049 body's LODs may draw vertex colors as normals), and its sheen layers
    // "~Lnn" + track2049::sheen_parts made from it (add_body_sheen); it takes the car's panel damage as a one-body
    // car's (car2049::add_livery_body: dents, and its texture loads, called from its lists, inlined per panel and
    // switched to scuffed copies, TrueColor::scuffed). The names sort after every other, so the name table stays
    // sorted. The game's loader relocates them as any model (once inlined, they load their textures in the list). STYLE n > 1 points the copy's FRAME1 lists
    // and sheen layers at livery n - 1's (a branch, no return) and empties its other body parts; STYLE 1 puts their
    // first commands back (paint_livery).
    constexpr int types_all = rush2::car_types::types;       // every car type (Rush 2's, 2049's, ...)
    std::map<int, std::vector<ModelMod>> liveries;           // by car asset index
    std::atomic<int> livery_counts[types_all] = {};          // by car type, for the car select's rows
    std::string livery_signature;
    // Texture table offsets (header word 2) of the containers given liveries: paint_livery finds a loaded copy's
    // start from its texture records with them. And each livery's body (car2049::add_livery_body id) by car asset and
    // livery (1-based), made again each time the asset is patched.
    std::mutex livery_tables_mutex;
    std::set<uint32_t> livery_tables;
    struct LiveryBody {
        int id = -1;
    };
    std::map<std::pair<int, int>, LiveryBody> livery_bodies;

    std::string livery_prefix(int v) {
        char p[8];
        snprintf(p, sizeof(p), "~L%02d", v);
        return p;
    }

    bool append_liveries(int index, std::vector<uint8_t>& out) {
        auto it = liveries.find(index);
        if (it == liveries.end() || it->second.empty() || out.size() < 0x28) return false;
        {
            std::lock_guard lock{ livery_tables_mutex };
            livery_tables.insert(be32(out, 8));
        }
        uint32_t models = be32(out, 0), names = be32(out, 4), count = be32(out, 16);
        if (count == 0 || count > 8192 || models + count * 0x34 > out.size() || names + count * 0x18 > out.size()) return false;
        // FRAME1's render state and LODs (what the lists are drawn under).
        int frame = -1;
        for (uint32_t i = 0; i < count && frame < 0; i++) {
            const char* n = reinterpret_cast<const char*>(&out[names + i * 0x18]);
            std::string name(n, strnlen(n, 15));
            if (name.size() >= 6 && name.compare(name.size() - 6, 6, "FRAME1") == 0) frame = int(i);
        }
        if (frame < 0) return false;
        uint32_t record = models + uint32_t(frame) * 0x34;
        uint32_t lods = std::clamp(be32(out, record), 1u, 4u);
        ListState st;
        bool lod_lit[4] = {};
        uint32_t lod_dist[4] = {};
        for (uint32_t l = 0; l < lods; l++) {
            uint32_t e = record + 4 + l * 12;
            lod_lit[l] = (((out[e + 2] << 8) | out[e + 3]) & 0x10) != 0;
            lod_dist[l] = be32(out, e + 4);
            if (l == 0 && be32(out, e + 8) < out.size()) list_state(out, be32(out, e + 8), st);
        }
        align8(out);
        uint32_t empty = uint32_t(out.size()); // the sheen parts' lists until add_body_sheen fills them
        add32(out, 0xDF000000);
        add32(out, 0);
        struct Added {
            std::string name;
            std::vector<uint8_t> record, entry;
        };
        std::vector<Added> added;
        auto add_model = [&](const std::string& name, uint32_t lod_count, const uint32_t* lists, const bool* lit,
                             float radius) {
            Added a{ name, std::vector<uint8_t>(0x34, 0), std::vector<uint8_t>(0x18, 0) };
            put32(a.record, 0, lod_count);
            for (uint32_t l = 0; l < lod_count; l++) {
                put16(a.record, 4 + l * 12 + 2, lit[l] ? 0x10 : 0);
                put32(a.record, 4 + l * 12 + 4, lod_dist[l]);
                put32(a.record, 4 + l * 12 + 8, lists[l]);
            }
            memcpy(a.entry.data(), name.data(), std::min<size_t>(15, name.size()));
            uint32_t bits;
            memcpy(&bits, &radius, 4);
            put32(a.entry, 16, bits);
            added.push_back(std::move(a));
        };
        // Per stand-in (shared by liveries): its texels, its texture-load list, its scuffed copy.
        std::map<uint64_t, std::pair<uint32_t, uint32_t>> texels_at; // content key -> texels, load list
        std::map<uint32_t, std::vector<uint8_t>> scuffs;               // texels -> scuffed copy
        uint32_t loads_start = 0, loads_end = 0;
        auto place = [&](const TrueColor& s) {
            auto t = texels_at.find(s.key);
            if (t != texels_at.end()) return t->second;
            align8(out);
            uint32_t at = uint32_t(out.size());
            out.insert(out.end(), s.texels.begin(), s.texels.end());
            align8(out);
            uint32_t load = uint32_t(out.size());
            for (auto [w0, w1] : true_color_load(s, at)) {
                add32(out, w0);
                add32(out, w1);
            }
            add32(out, 0xDF000000);
            add32(out, 0);
            if (loads_start == 0) loads_start = load;
            loads_end = uint32_t(out.size());
            scuffs[at] = s.scuffed;
            return texels_at.emplace(s.key, std::make_pair(at, load)).first->second;
        };
        for (size_t v = 0; v < it->second.size(); v++) {
            const ModelMod& mm = it->second[v];
            std::vector<uint32_t> texels, loads;
            for (const TrueColor& s : mm.stand_ins) {
                auto [at, load] = place(s);
                texels.push_back(at);
                loads.push_back(load);
            }
            // Each LOD its own list: the loader rebases a list once per LOD that names it.
            float radius = 0.0f;
            uint32_t lists[4];
            for (uint32_t l = 0; l < lods; l++) lists[l] = emit_model(out, mm, texels, st, lod_lit[l], radius, &loads);
            std::string prefix = livery_prefix(int(v) + 1);
            add_model(prefix + "FRAME1", lods, lists, lod_lit, radius / 16.0f);
            const bool unlit[1] = { false };
            for (const char* part : rush2::track2049::sheen_parts) add_model(prefix + part, 1, &empty, unlit, 0.5f);
            // The hood camera's hood, as a converted 2049 car's (its HOOD): H0_L, and H2_L too (Rush 2 swaps it in by
            // panel 0's damage), each its own list, loading its textures itself; add_body_sheen makes its sheen layers
            // H0D_L / H2D_L from H0_L.
            if (mm.hood) {
                std::vector<uint32_t> hood_texels;
                for (const TrueColor& s : mm.hood->stand_ins) hood_texels.push_back(place(s).first);
                const bool hood_lit[1] = { lod_lit[0] };
                float hood_radius = 0.0f;
                for (const char* part : { "H0_L", "H2_L" }) {
                    uint32_t list = emit_model(out, *mm.hood, hood_texels, st, lod_lit[0], hood_radius);
                    add_model(prefix + part, 1, &list, hood_lit, hood_radius / 16.0f);
                }
                for (const char* part : { "H0D_L", "H2D_L" }) add_model(prefix + part, 1, &empty, unlit, 0.5f);
            }
            printf("[livery] %s (asset 0x%X) from %s: %zu triangles\n", mm.file.c_str(), index, mm.mod_id.c_str(),
                   mm.triangles);
        }
        std::sort(added.begin(), added.end(), [](const Added& a, const Added& b) {
            return strncmp(a.name.c_str(), b.name.c_str(), 15) < 0;
        });
        std::vector<uint8_t> new_models(out.begin() + models, out.begin() + models + count * 0x34);
        std::vector<uint8_t> new_names(out.begin() + names, out.begin() + names + count * 0x18);
        for (const Added& a : added) {
            new_models.insert(new_models.end(), a.record.begin(), a.record.end());
            new_names.insert(new_names.end(), a.entry.begin(), a.entry.end());
        }
        align8(out);
        put32(out, 0, uint32_t(out.size()));
        out.insert(out.end(), new_models.begin(), new_models.end());
        align8(out);
        put32(out, 4, uint32_t(out.size()));
        out.insert(out.end(), new_names.begin(), new_names.end());
        put32(out, 16, count + uint32_t(added.size()));
        // Each livery's sheen layers, made from it as a new car's are, then its body for denting.
        for (size_t v = 0; v < it->second.size(); v++) {
            std::string error;
            if (!rush2::track2049::add_body_sheen(out, livery_prefix(int(v) + 1), error)) {
                fprintf(stderr, "[livery] %s: no sheen: %s\n", it->second[v].file.c_str(), error.c_str());
            }
        }
        std::lock_guard lock{ livery_tables_mutex };
        for (size_t v = 0; v < it->second.size(); v++) {
            LiveryBody& b = livery_bodies[{ index, int(v) + 1 }];
            b.id = rush2::car2049::add_livery_body(b.id, out, livery_prefix(int(v) + 1), scuffs, loads_start, loads_end);
        }
        return true;
    }

    // STYLE choices: {"<profile record address, hex>": {"<car type>": style}} in the saves' "liveries" section.
    const std::string livery_section = "liveries";
    std::mutex style_mutex;
    std::map<uint32_t, std::map<int, int>> styles;
    bool styles_loaded = false;

    void load_styles() {
        if (styles_loaded) return;
        styles_loaded = true;
        nlohmann::json j = nlohmann::json::parse(rush2::data_files::read(rush2::data_files::File::Saves, livery_section),
                                                 nullptr, false);
        if (!j.is_object()) return;
        for (auto p = j.begin(); p != j.end(); ++p) {
            uint32_t profile = uint32_t(std::strtoul(p.key().c_str(), nullptr, 16));
            if (!p.value().is_object()) continue;
            for (auto t = p.value().begin(); t != p.value().end(); ++t) {
                if (t.value().is_number_integer()) styles[profile][std::atoi(t.key().c_str())] = t.value().get<int>();
            }
        }
    }

    // A loaded copy's body lists' first commands as they loaded (list address -> words), with the copy they came
    // from (its address and name table), so a list loaded over by another car isn't given them back; and the words
    // a livery put there (list address -> words). Guarded by style_mutex.
    struct Original {
        uint32_t w0, w1, copy, names;
    };
    std::map<uint32_t, Original> originals;
    std::map<uint32_t, std::pair<uint32_t, uint32_t>> livery_words;
    bool lifted = false;  // livery_words words taken off for the mirror walk (rush2_livery_mirror_begin)
    // The livery body each copy shows (copy base -> car2049::add_livery_body id, and the first FRAME1 list with the
    // branch to it there, which tells a copy still showing it from one loaded over).
    struct Shown {
        int body;
        uint32_t list, w1;
    };
    std::map<uint32_t, Shown> shown;

    // The loaded copy of car `id` (a preview id or race car) when its container has liveries, else 0: its texture
    // bank's records (as func_8008582C finds them) less the container's texture table offset, which its header holds
    // at +8.
    uint32_t livery_copy(uint8_t* rdram, int id) {
        constexpr uint32_t bank_table = 0x80119220;  // [bank] = the loaded copy's texture records, count
        if (id < 0 || id >= rush2::car_types::preview_ids) return 0;
        int bank = (int16_t)MEM_H(0, (int32_t)(rush2::car_types::car_banks + id * 2));
        if (bank < 0 || bank >= (int)MEM_BU(0, (int32_t)0x800D5788)) return 0;
        uint32_t records = (uint32_t)MEM_W(0, (int32_t)(bank_table + bank * 8));
        std::lock_guard lock{ livery_tables_mutex };
        for (uint32_t offset : livery_tables) {
            uint32_t at = records - offset;
            if (at >= 0x80000000u && at < 0x80800000u && (uint32_t)MEM_W(8, (int32_t)at) == offset) return at;
        }
        return 0;
    }

    void write_words(uint8_t* rdram, uint32_t list, uint32_t w0, uint32_t w1) {
        MEM_W(0, (int32_t)list) = (int32_t)w0;
        MEM_W(4, (int32_t)list) = (int32_t)w1;
    }
}

// Start of func_800776DC, the mirror walk of an asset slot: the lists as they loaded while it turns triangles over.
extern "C" void rush2_livery_mirror_begin(uint8_t* rdram, recomp_context* ctx) {
    std::lock_guard lock{ style_mutex };
    if (livery_words.empty()) return;
    for (const auto& [list, words] : livery_words) {
        auto o = originals.find(list);
        if (o == originals.end()) continue;
        if ((uint32_t)MEM_W(0, (int32_t)list) == words.first && (uint32_t)MEM_W(4, (int32_t)list) == words.second) {
            write_words(rdram, list, o->second.w0, o->second.w1);
        }
    }
    lifted = true;
}

// Its exit (0x800777E8): the liveries' words back.
extern "C" void rush2_livery_mirror_end(uint8_t* rdram, recomp_context* ctx) {
    std::lock_guard lock{ style_mutex };
    if (!lifted) return;
    lifted = false;
    for (auto it = livery_words.begin(); it != livery_words.end();) {
        auto o = originals.find(it->first);
        if (o != originals.end() && (uint32_t)MEM_W(0, (int32_t)it->first) == o->second.w0 &&
            (uint32_t)MEM_W(4, (int32_t)it->first) == o->second.w1) {
            write_words(rdram, it->first, it->second.first, it->second.second);
            ++it;
        }
        else {
            it = livery_words.erase(it); // loaded over meanwhile
        }
    }
}

int rush2::car_mods::livery_count(int type) {
    return type >= 0 && type < types_all ? livery_counts[type].load(std::memory_order_relaxed) : 0;
}

int rush2::car_mods::livery_style(uint8_t* rdram, uint32_t profile, int type) {
    int n = livery_count(type);
    if (n == 0 || profile == 0) return 0;
    std::lock_guard lock{ style_mutex };
    load_styles();
    auto p = styles.find(profile);
    if (p == styles.end()) return 0;
    auto t = p->second.find(type);
    return t == p->second.end() ? 0 : std::clamp(t->second, 0, n);
}

void rush2::car_mods::set_livery_style(uint8_t* rdram, uint32_t profile, int type, int style) {
    if (profile == 0 || type < 0 || type >= types_all) return;
    std::lock_guard lock{ style_mutex };
    load_styles();
    styles[profile][type] = style;
    nlohmann::json j = nlohmann::json::object();
    for (const auto& [record, by_type] : styles) {
        char key[16];
        snprintf(key, sizeof(key), "%08X", record);
        for (const auto& [t, v] : by_type) {
            if (v != 0) j[key][std::to_string(t)] = v;
        }
    }
    rush2::data_files::write(rush2::data_files::File::Saves, livery_section, j.dump());
}

int rush2::car_mods::livery_body(uint8_t* rdram, int id, uint32_t& base) {
    uint32_t copy = livery_copy(rdram, id);
    if (copy == 0) return -1;
    std::lock_guard lock{ style_mutex };
    auto s = shown.find(copy);
    if (s == shown.end()) return -1;
    // The branch still in place (not loaded over, nor taken off for the mirror walk).
    if ((uint32_t)MEM_W(0, (int32_t)s->second.list) != 0xDE010000u || (uint32_t)MEM_W(4, (int32_t)s->second.list) != s->second.w1) {
        return -1;
    }
    base = copy;
    return s->second.body;
}

void rush2::car_mods::paint_livery(uint8_t* rdram, int id) {
    constexpr uint32_t car_table = 0x800D9C10;   // race cars: 9 bytes each, +1 type, +7 0xB0 a human's
    int type = -1;
    uint32_t profile = 0;
    if (MEM_W(0, (int32_t)game_state) == 2) {     // the car select: id = panel x types + type
        if (id < 0 || id >= rush2::car_types::preview_ids) return;
        // Two players choose in panels 0 and 1; one player's car shows in panel 1 (found in game: Camaro id 78).
        int slot = MEM_H(0, (int32_t)race_player_count) == 1 ? 0 : id / types_all;
        type = id % types_all;
        int player = slot + (rush2::players4::car_select_second_round() ? 2 : 0);
        profile = (uint32_t)MEM_W(0x24, rush2::players4::record(player));
    }
    else {                                        // a race car: a human's is its player's ("car = player", +0)
        constexpr uint32_t physics_cars = 0x800F5470; // 0x81C each: +0x7E8 2 a human's, +0x7EA its type
        if (id < 0 || id >= 8) return;
        uint32_t car = physics_cars + id * 0x81C;
        bool human = MEM_BU(0, (int32_t)(car + 0x7E8)) == 2;
        type = human ? MEM_BU(0, (int32_t)(car + 0x7EA)) : MEM_BU(0, (int32_t)(car_table + id * 9 + 1));
        if (human) {
            for (int p = 0; p < 4; p++) {
                if (MEM_BU(0, rush2::players4::record(p)) == id) profile = (uint32_t)MEM_W(0x24, rush2::players4::record(p));
            }
        }
    }
    if (livery_count(type) == 0) return;
    int style = livery_style(rdram, profile, type);
    uint32_t copy = livery_copy(rdram, id);
    if (copy == 0) return;
    uint32_t models = copy + (uint32_t)MEM_W(0, (int32_t)copy), names = copy + (uint32_t)MEM_W(4, (int32_t)copy);
    uint32_t count = (uint32_t)MEM_W(16, (int32_t)copy);
    if (count == 0 || count > 8192) return;
    auto name_of = [&](uint32_t i) {
        char n[16] = {};
        for (int k = 0; k < 15; k++) n[k] = (char)MEM_B(0, (int32_t)(names + i * 0x18 + k));
        return std::string(n);
    };
    auto lods_of = [&](uint32_t i) { return std::min((uint32_t)MEM_W(0, (int32_t)(models + i * 0x34)), 4u); };
    auto list_of = [&](uint32_t i, uint32_t l) { return 0x80000000u | ((uint32_t)MEM_W(0, (int32_t)(models + i * 0x34 + 4 + l * 12 + 8)) & 0xFFFFFF); };
    auto ends_with = [](const std::string& n, const char* part) {
        size_t len = strlen(part);
        return n.size() > len && n.compare(n.size() - len, len, part) == 0;
    };
    // The picked livery's models by part (its "~Lnn" + FRAME1, sheen layers, hood: the parts it has its own of), and
    // every livery list's address (what a patched list branches to).
    std::string prefix = style > 0 ? livery_prefix(style) : std::string();
    std::map<std::string, uint32_t> livery_parts;
    std::set<uint32_t> livery_lists;
    std::vector<std::pair<uint32_t, const char*>> body; // the car's part models, and which part
    for (uint32_t i = 0; i < count; i++) {
        std::string n = name_of(i);
        if (n.rfind("~L", 0) == 0) {
            for (uint32_t l = 0; l < lods_of(i); l++) livery_lists.insert(list_of(i, l) & 0xFFFFFF);
            if (style > 0 && n.rfind(prefix, 0) == 0) livery_parts[n.substr(prefix.size())] = i;
            continue;
        }
        for (const char* part : parts) {
            if (ends_with(n, part)) {
                body.push_back({ i, part });
                break;
            }
        }
    }
    auto livery_frame = livery_parts.find("FRAME1");
    if (livery_lists.empty() || (style > 0 && livery_frame == livery_parts.end())) return;
    // What each of the car's part lists starts with: a branch to the livery's own of that part (by LOD), else nothing;
    // a list two parts share takes the branch.
    std::map<uint32_t, uint32_t> branch_to; // list -> livery list (0: empty it)
    for (const auto& [i, part] : body) {
        auto own = style > 0 ? livery_parts.find(part) : livery_parts.end();
        for (uint32_t l = 0; l < lods_of(i); l++) {
            uint32_t list = list_of(i, l);
            if (list < 0x80000000u + 0x400 || list >= 0x80800000u) continue;
            uint32_t to = own != livery_parts.end() ? list_of(own->second, std::min(l, lods_of(own->second) - 1)) : 0;
            uint32_t& t = branch_to[list];
            if (to != 0) t = to;
        }
    }
    std::lock_guard lock{ style_mutex };
    shown.erase(copy);
    Shown now{ -1, 0, 0 };
    for (const auto& [list, to] : branch_to) {
        uint32_t w0 = (uint32_t)MEM_W(0, (int32_t)list), w1 = (uint32_t)MEM_W(4, (int32_t)list);
        bool ours = (w0 == 0xDF000000u && w1 == 0) || (w0 == 0xDE010000u && livery_lists.count(w1 & 0xFFFFFF));
        auto o = originals.find(list);
        if (o != originals.end() && (o->second.copy != copy || o->second.names != names)) {
            originals.erase(o);
            o = originals.end();
        }
        if (!ours) o = originals.insert_or_assign(list, Original{ w0, w1, copy, names }).first;
        if (o == originals.end()) {
            livery_words.erase(list); // empty as it loaded: it stays so
            continue;
        }
        if (style == 0) {
            write_words(rdram, list, o->second.w0, o->second.w1);
            livery_words.erase(list);
            continue;
        }
        uint32_t n0 = to != 0 ? 0xDE010000u : 0xDF000000u, n1 = to != 0 ? (to & 0xFFFFFF) : 0;
        write_words(rdram, list, n0, n1);
        livery_words[list] = { n0, n1 };
        if (to != 0 && now.list == 0 && to == list_of(livery_frame->second, 0)) now = Shown{ -1, list, n1 };
    }
    if (style > 0 && now.list != 0) {
        int index = type < types ? first_car_asset + type : rush2::car2049::car_asset(type);
        std::lock_guard tables{ livery_tables_mutex };
        auto b = livery_bodies.find({ index, style });
        if (b != livery_bodies.end() && b->second.id >= 0) {
            now.body = b->second.id;
            shown[copy] = now;
        }
    }
}

namespace {
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
    constexpr int new_car_slots = rush2::car_types::count_mod;
    Applied applied_new[new_car_slots]; // the new cars (rush2/new_cars), by slot
    std::string applied_new_key[new_car_slots]; // mod id, car id and the hash of its files
    bool has_own[types] = {};
    std::vector<uint8_t> own[types];     // each type's asset before its mod (a recomp fix, or empty: the ROM's)
    std::string last_signature;
    std::atomic<bool> reload_requested = false;   // rush2::car_mods::reload
    bool initialized = false;
    int frames = 0;

    using rush2::mod_files::ModFile;
    using rush2::mod_files::enabled_mods;
    using rush2::mod_files::read_mod_file;

    // While a ZipBatch lives on a thread, read_mod_file and zip_of there open each mod's .rtz once: a zip's central
    // directory is read and sorted on every open, and a plan reads dozens of files (one open per file read took most
    // of the 270 ms the boot spent on car mods with five mods).
    thread_local std::map<std::filesystem::path, std::unique_ptr<rush2::upscale::ZipReader>>* open_zips = nullptr;
    struct ZipBatch {
        std::map<std::filesystem::path, std::unique_ptr<rush2::upscale::ZipReader>> zips;
        ZipBatch() { open_zips = &zips; }
        ~ZipBatch() { open_zips = nullptr; }
    };

    // The mod's .rtz, opened once in the thread's batch (is_open false for a folder mod).
    rush2::upscale::ZipReader& zip_of(const std::filesystem::path& path) {
        std::unique_ptr<rush2::upscale::ZipReader>& z = (*open_zips)[path];
        if (!z) z = std::make_unique<rush2::upscale::ZipReader>(path);
        return *z;
    }

    // ---- plan --------------------------------------------------------------------------------------------------
    // What apply installs, read and converted from the enabled mods' files with no game memory, so a background thread
    // (rush2::car_mods::prepare, src/prewarm.cpp) can make it ahead of the boot or after the mods change, and the game
    // thread only installs it.
    struct NewCarPlan {
        int n = 0;                      // its mod car slot
        std::string id, mod_id, key;    // key: mod id, car id and the hash of its files
        uint64_t hash = 0;
        rush2::car2049::ModCar spec;
        std::shared_ptr<Model> model;
        std::vector<uint8_t> car;
        std::vector<StandIn> stand_ins;
    };
    struct ReplacementPlan {
        enum State { none, failed, ok } state = none;  // no mod has the car, one has it but it can't be converted, converted
        std::string mod_id;
        uint64_t hash = 0;
        std::shared_ptr<Model> model;
        std::vector<uint8_t> car;
        std::vector<StandIn> stand_ins;
    };
    struct Plan {
        std::string key;                // plan_key of the mods it was made from
        std::string model_signature;
        std::map<std::string, ModelMod> models;
        std::string livery_signature;
        std::map<int, std::vector<ModelMod>> liveries;
        std::array<int, types_all> livery_counts{};
        std::vector<NewCarPlan> new_cars;
        int slots_used = 0;             // mod car slots the new cars fill (some may have failed to convert after)
        std::array<ReplacementPlan, types> replacements;
    };

    // The car from glbs (a new car's color schemes, in order; one for a replaced car), its parts named `prefix`: its
    // container (cached as `cache_name`) and stand-ins (scuffed by `seed`). sheen: also Rush 2's sheen layers made from
    // the body (rush2::track2049::add_body_sheen, as California Speed's cars get them). False on a bad file.
    // hoods: the hood camera's hood of each scheme (glbs too; empty: none), drawn as H0_L / H2_L.
    bool convert(const std::string& prefix, const std::string& cache_name, uint32_t seed,
                 const std::vector<std::vector<uint8_t>>& glbs, std::shared_ptr<Model>& model, std::vector<uint8_t>& car,
                 std::vector<StandIn>& stand_ins, std::string& error, bool sheen = false,
                 const std::vector<std::vector<uint8_t>>& hoods = {}) {
        model = std::make_shared<Model>();
        std::vector<uint8_t> pal;
        for (const Rgba& c : car_palette) pal.insert(pal.end(), { c.r, c.g, c.b, c.a });
        uint64_t source = rush2::track_cache::hash(pal.data(), pal.size(), cache_version) ^ (sheen ? 0x5EE7ull : 0);
        for (size_t k = 0; k < glbs.size(); k++) {
            size_t before = model->parts.size();
            if (!parse_glb(glbs[k], *model, error)) return false;
            if (model->parts.size() == before) {
                error = "scheme " + std::to_string(k + 1) + " has no triangles";
                return false;
            }
            for (size_t i = before; i < model->parts.size(); i++) model->parts[i].scheme = int(k);
            source = rush2::track_cache::hash(glbs[k].data(), glbs[k].size(), source);
        }
        for (size_t k = 0; k < hoods.size(); k++) {
            if (hoods[k].empty()) continue;
            size_t before = model->parts.size();
            if (!parse_glb(hoods[k], *model, error)) return false;
            for (size_t i = before; i < model->parts.size(); i++) {
                model->parts[i].scheme = int(k);
                model->parts[i].hood = true;
            }
            source = rush2::track_cache::hash(hoods[k].data(), hoods[k].size(), source ^ 0x400Dull);
        }
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
                make_stand_in(*model, s, uint32_t(seed * 977 + stand_ins.size()));
                stand_ins.push_back(std::move(s));
            }
        }
        car = build_container(prefix, *model, stand_ins);
        if (sheen && !rush2::track2049::add_body_sheen(car, prefix, error)) return false;
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
            rush2::upscale::ZipReader& zip = zip_of(mods[i].path);
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

    // The model mods of the enabled mods, into the plan.
    void load_models(const std::vector<ModFile>& mods, Plan& plan) {
        std::vector<ModelFile> files = model_files(mods);
        std::string signature;
        for (const ModelFile& f : files) {
            std::error_code ec;
            auto time = std::filesystem::last_write_time(mods[f.mod].path, ec);
            signature += mods[f.mod].mod_id + ":" + f.path + ":" + std::to_string(ec ? 0 : time.time_since_epoch().count()) + ";";
        }
        plan.model_signature = signature;
        std::map<std::string, ModelMod>& loaded = plan.models;
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
        for (const auto& [index, list] : liveries) {
            std::vector<const ModelMod*> all;
            for (const ModelMod& mm : list) {
                all.push_back(&mm);
                if (mm.hood) all.push_back(mm.hood.get());
            }
            for (const ModelMod* m : all) {
                const ModelMod& mm = *m;
                for (const TrueColor& s : mm.stand_ins) {
                    std::shared_ptr<const Model> model = mm.model;
                    int image = s.image;
                    rush2::derive::ModTexture mt;
                    mt.id = mm.glb_hash ^ (uint64_t(uint16_t(image)) << 48) ^ 0x6C69766572ull;
                    mt.load = [model, image](Image& out, Image&) {
                        return image >= 0 && rush2::upscale::decode_image(model->images[image], out);
                    };
                    mt.seed = uint32_t(mm.glb_hash) * 977u + uint32_t(image);
                    textures[s.key] = mt;
                    if (!s.scuffed.empty()) {
                        mt.damaged = true;
                        textures[s.scuffed_key] = mt;
                    }
                }
            }
        }
        // Rush 2's cars as t, the new cars as 64 + their slot.
        for (int t = 0; t < types + new_car_slots; t++) {
            const Applied& a = t < types ? applied[t] : applied_new[t - types];
            uint32_t id = t < types ? uint32_t(t) : uint32_t(64 + t - types);
            if (!a.model) continue;
            for (const StandIn& s : a.stand_ins) {
                std::shared_ptr<const Model> model = a.model;
                int image = s.image, mask = s.mask;
                rush2::derive::ModTexture mt;
                mt.id = (uint64_t(id) << 32) | (uint64_t(uint16_t(image)) << 16) | uint16_t(mask);
                mt.load = [model, image, mask](Image& out, Image& out_mask) {
                    if (image < 0 || !rush2::upscale::decode_image(model->images[image], out)) return false;
                    if (mask >= 0) decode_mask(model->images[mask], out_mask);
                    return true;
                };
                mt.main_entry = main_first;
                mt.accent_entry = accent_first;
                mt.seed = uint32_t(id * 977 + image);
                textures[s.key] = mt;
                if (s.scuffed_key != s.key) { // no paint, nothing to scuff (a glass color)
                    mt.damaged = true;
                    textures[s.scuffed_key] = mt;
                }
            }
        }
        rush2::derive::set_mod_textures("cars", std::move(textures));
    }

    // The enabled mods' rush2/liveries/<ID>.json, in mod order and then by ID, into the plan.
    void load_liveries(const std::vector<ModFile>& mods, Plan& plan) {
        std::string signature;
        std::vector<std::pair<size_t, std::string>> files; // mod, path
        const std::string prefix = "rush2/liveries/";
        for (size_t i = 0; i < mods.size(); i++) {
            std::vector<std::string> found;
            auto add = [&](const std::string& path) {
                if (path.rfind(prefix, 0) != 0 || path.size() <= prefix.size() + 5) return;
                std::string file = path.substr(prefix.size());
                if (file.find('/') == std::string::npos && upper(file.substr(file.size() - 5)) == ".JSON") found.push_back(path);
            };
            rush2::upscale::ZipReader& zip = zip_of(mods[i].path);
            if (zip.is_open()) {
                for (const std::string& n : zip.names()) add(n);
            }
            else {
                std::error_code ec;
                std::filesystem::path dir = mods[i].path / "rush2" / "liveries";
                for (auto it = std::filesystem::directory_iterator(dir, ec); !ec && it != std::filesystem::directory_iterator();
                     it.increment(ec)) {
                    std::u8string name = it->path().filename().u8string();
                    add(prefix + std::string(name.begin(), name.end()));
                }
            }
            std::sort(found.begin(), found.end());
            for (const std::string& f : found) {
                files.emplace_back(i, f);
                std::error_code ec;
                auto time = std::filesystem::last_write_time(mods[i].path, ec);
                signature += mods[i].mod_id + ":" + f + ":" + std::to_string(ec ? 0 : time.time_since_epoch().count()) + ";";
            }
        }
        plan.livery_signature = signature;
        std::map<int, std::vector<ModelMod>>& loaded = plan.liveries;
        std::array<int, types_all>& counts = plan.livery_counts;
        for (const auto& [mod, path] : files) {
            const ModFile& m = mods[mod];
            std::vector<uint8_t> text;
            std::string error;
            nlohmann::json j;
            if (read_mod_file(m, path, text)) j = nlohmann::json::parse(text.begin(), text.end(), nullptr, false);
            std::string car = j.is_object() ? upper(j.value("car", std::string())) : std::string();
            int type = -1;
            for (int t = 0; t < types; t++) {
                if (car == prefixes[t]) type = t;
            }
            if (type < 0 && rush2::car2049::car_index(car) >= 0) type = rush2::car2049::first_type + rush2::car2049::car_index(car);
            if (!j.is_object() || type < 0 || !j.contains("styles") || !j["styles"].is_array()) {
                fprintf(stderr, "[livery] %s in %s: needs \"car\" (a Rush 2 car's prefix or a Rush 2049 car) and \"styles\"\n",
                        path.c_str(), m.mod_id.c_str());
                continue;
            }
            int index = type < types ? first_car_asset + type : rush2::car2049::car_asset(type);
            // The hood camera's hood of each style (H0_L / H2_L, as a Rush 2049 car's HOOD): "hoods", by style.
            const nlohmann::json hoods = j.contains("hoods") && j["hoods"].is_array() ? j["hoods"] : nlohmann::json::array();
            for (size_t si = 0; si < j["styles"].size(); si++) {
                const auto& f = j["styles"][si];
                std::string file = f.is_string() ? f.get<std::string>() : std::string();
                std::vector<uint8_t> glb;
                ModelMod mm;
                mm.mod_id = m.mod_id;
                mm.file = prefix + file;
                if (counts[type] + 1 >= rush2::car2049::max_schemes) {
                    fprintf(stderr, "[livery] %s in %s: %s has %d styles already\n", mm.file.c_str(), m.mod_id.c_str(),
                            car.c_str(), rush2::car2049::max_schemes);
                    break;
                }
                if (file.empty() || file.find_first_of("/\\") != std::string::npos || !read_mod_file(m, mm.file, glb) ||
                    !load_model_mod(glb, mm, error, true)) {
                    fprintf(stderr, "[livery] %s in %s: %s\n", mm.file.c_str(), m.mod_id.c_str(),
                            error.empty() ? "unreadable" : error.c_str());
                    continue;
                }
                std::string hood_file = si < hoods.size() && hoods[si].is_string() ? hoods[si].get<std::string>() : "";
                if (!hood_file.empty()) {
                    auto hood = std::make_shared<ModelMod>();
                    hood->mod_id = m.mod_id;
                    hood->file = prefix + hood_file;
                    std::vector<uint8_t> hglb;
                    error.clear();
                    if (hood_file.find_first_of("/\\") == std::string::npos && read_mod_file(m, hood->file, hglb) &&
                        load_model_mod(hglb, *hood, error, true)) {
                        mm.hood = hood;
                    }
                    else {
                        fprintf(stderr, "[livery] %s in %s: %s\n", hood->file.c_str(), m.mod_id.c_str(),
                                error.empty() ? "unreadable" : error.c_str());
                    }
                }
                loaded[index].push_back(std::move(mm));
                counts[type]++;
            }
        }
    }

    // ---- new cars -------------------------------------------------------------------------------------------------
    // A mod's rush2/new_cars/<ID>.json and <ID>.glb add a car (docs/car_mods.md#new-cars): the enabled mods' cars, in
    // mod order and then by ID, fill the 16 mod car types (include/car_types.h) in turn.
    struct NewCarFile {
        std::string id;  // <ID>, as the mod spells it
        size_t mod;
    };

    std::vector<NewCarFile> new_car_files(const std::vector<ModFile>& mods) {
        std::vector<NewCarFile> out;
        std::set<std::string> seen;
        const std::string prefix = "rush2/new_cars/";
        for (size_t i = 0; i < mods.size(); i++) {
            std::vector<std::string> ids;
            auto add = [&](const std::string& path) {
                if (path.rfind(prefix, 0) != 0 || path.size() <= prefix.size() + 5) return;
                std::string file = path.substr(prefix.size());
                if (file.find('/') != std::string::npos || upper(file.substr(file.size() - 5)) != ".JSON") return;
                ids.push_back(file.substr(0, file.size() - 5));
            };
            rush2::upscale::ZipReader& zip = zip_of(mods[i].path);
            if (zip.is_open()) {
                for (const std::string& n : zip.names()) add(n);
            }
            else {
                std::error_code ec;
                std::filesystem::path dir = mods[i].path / "rush2" / "new_cars";
                for (auto it = std::filesystem::directory_iterator(dir, ec); !ec && it != std::filesystem::directory_iterator();
                     it.increment(ec)) {
                    std::u8string name = it->path().filename().u8string();
                    add(prefix + std::string(name.begin(), name.end()));
                }
            }
            std::sort(ids.begin(), ids.end());
            for (const std::string& id : ids) {
                if (seen.insert(mods[i].mod_id + "/" + upper(id)).second) out.push_back({ id, i });
            }
        }
        return out;
    }

    // The car's settings from its .json; false (with the reason) when they can't be used.
    // A new car's .json. schemes: its glbs (file names in rush2/new_cars/), the "schemes" list, empty without one.
    // hoods: the "hoods" list (the hood camera's hood per scheme, or of the car), empty without one.
    bool read_new_car(const std::vector<uint8_t>& text, rush2::car2049::ModCar& spec, std::vector<std::string>& schemes,
                      std::vector<std::string>& hoods, std::string& error) {
        nlohmann::json j = nlohmann::json::parse(text.begin(), text.end(), nullptr, false);
        if (!j.is_object()) {
            error = "the .json isn't a JSON object";
            return false;
        }
        // The name, in the car select logo's letters (A-Z, 0-9, space and '-'), at most 12 of them.
        std::string name;
        for (char c : j.value("name", std::string())) {
            char u = char(std::toupper((unsigned char)c));
            if ((u >= 'A' && u <= 'Z') || (u >= '0' && u <= '9') || u == ' ' || u == '-') name += u;
        }
        if (name.size() > 12) name.resize(12);
        if (name.empty()) {
            error = "\"name\" is missing";
            return false;
        }
        spec.name = name;
        std::string base = upper(j.value("base", std::string("PICKUP")));
        spec.base = -1;
        for (int t = 0; t < types; t++) {
            if (base == prefixes[t]) spec.base = t;
        }
        if (spec.base < 0) {
            error = "\"base\" " + base + " isn't a Rush 2 car (PICKUP, VETTE, HOTROD, ...)";
            return false;
        }
        auto vec = [&](const nlohmann::json& v, float* out, size_t n) {
            if (!v.is_array() || v.size() != n) return false;
            for (size_t i = 0; i < n; i++) {
                if (!v[i].is_number()) return false;
                out[i] = v[i].get<float>();
            }
            return true;
        };
        // Wheel centers in the glb's space (vertex units of 1/16 ft, the model dump's x), the right side's: the left
        // wheels mirror them. The descriptor's point is a foot below the center (the tire's radius,
        // carselect_preview_update_8005A598), and its x is the game's (the right side positive).
        if (j.contains("wheels")) {
            float front[3], rear[3];
            if (!j["wheels"].is_object() || !vec(j["wheels"].value("front", nlohmann::json()), front, 3) ||
                !vec(j["wheels"].value("rear", nlohmann::json()), rear, 3)) {
                error = "\"wheels\" needs \"front\" and \"rear\", each [x, y, z]";
                return false;
            }
            for (int w = 0; w < 4; w++) {
                const float* c = w < 2 ? front : rear;
                spec.wheels[w][0] = (w % 2 == 0 ? 1.0f : -1.0f) * std::fabs(c[0]) / 16.0f;
                spec.wheels[w][1] = c[1] / 16.0f - 1.0f;
                spec.wheels[w][2] = c[2] / 16.0f;
            }
            spec.has_wheels = true;
        }
        if (j.contains("wheel_scale") && !vec(j["wheel_scale"], spec.wheel_scale, 2)) {
            error = "\"wheel_scale\" needs [front, rear]";
            return false;
        }
        for (float& s : spec.wheel_scale) s = std::clamp(s, 0.0f, 3.0f);
        if (j.contains("box")) {
            if (!vec(j["box"], spec.box, 4)) {
                error = "\"box\" needs [front, rear, half width, height] in feet";
                return false;
            }
            spec.has_box = true;
        }
        auto color = [&](const char* key) {
            int c = j.value(key, -1);
            return c >= 0 && c < rush2::paint_colors::rush2_colors ? c : -1;
        };
        spec.main = color("main_color");
        spec.accent = color("accent_color");
        // A Rush 2049 car whose physics the car takes in place of the base car's (docs/car_mods.md#physics).
        if (j.contains("physics_from")) {
            std::string from = j["physics_from"].is_string() ? j["physics_from"].get<std::string>() : std::string();
            spec.physics_from = rush2::car2049::car_index(from);
            if (spec.physics_from < 0) {
                error = "\"physics_from\" " + from + " isn't a Rush 2049 car (BRUISER, 8-BALL, ...)";
                return false;
            }
        }
        // Physics (docs/car_mods.md#physics), in Rush 2's units: each key optional, the base car's value without it.
        if (j.contains("physics")) {
            const auto& p = j["physics"];
            if (!p.is_object()) {
                error = "\"physics\" needs to be an object";
                return false;
            }
            namespace ct = rush2::car_types;
            // Descriptor fields: key, offset, count.
            struct DescKey { const char* key; uint32_t offset; size_t count; };
            static constexpr DescKey desc_keys[] = {
                { "inertia_constants", 0x00, 3 }, { "suspension", 0x18, 14 }, { "suspension_constants", 0x50, 2 },
                { "drag", 0x60, 1 }, { "rolling_resistance", 0x64, 1 }, { "rear_grip", 0x78, 1 },
                { "drivetrain_inertia", 0xAC, 1 }, { "final_drive", 0xB0, 1 }, { "clutch", 0xB4, 1 },
                { "torque", 0xB8, 4 }, { "gears", 0xC8, 6 }, { "shift_up", 0xE4, 1 }, { "shift_down", 0xE8, 1 },
            };
            struct TableKey { const char* key; uint32_t table; bool byte; };
            static constexpr TableKey table_keys[] = {
                { "mass", ct::mass, false }, { "pitch_roll_inertia", ct::inertia, false },
                { "yaw_inertia", ct::yaw_inertia, false }, { "preload", ct::preload, false },
                { "steering", ct::steer_base, false }, { "yaw_damping", ct::yaw_damping, false },
                { "durability", ct::weight, false }, { "drive", ct::drive, true },
                { "suspension_setting", ct::suspension, true }, { "tires_setting", ct::tires, true },
            };
            for (auto it = p.begin(); it != p.end(); ++it) {
                const std::string& key = it.key();
                const auto& v = it.value();
                bool known = false;
                for (const DescKey& d : desc_keys) {
                    if (key != d.key) continue;
                    known = true;
                    float vals[14];
                    if (d.count == 1 ? !v.is_number() : !vec(v, vals, d.count)) {
                        error = "\"physics\" \"" + key + "\" needs " +
                                (d.count == 1 ? std::string("a number") : std::to_string(d.count) + " numbers");
                        return false;
                    }
                    if (d.count == 1) vals[0] = v.get<float>();
                    for (size_t i = 0; i < d.count; i++) spec.desc_floats.emplace_back(d.offset + 4 * uint32_t(i), vals[i]);
                    if (d.offset == 0x00) { // and their inverses, which the game reads too
                        for (size_t i = 0; i < 3; i++) {
                            spec.desc_floats.emplace_back(0x0C + 4 * uint32_t(i), vals[i] != 0.0f ? 1.0f / vals[i] : 0.0f);
                        }
                    }
                }
                for (const TableKey& t : table_keys) {
                    if (key != t.key) continue;
                    known = true;
                    if (!v.is_number()) {
                        error = "\"physics\" \"" + key + "\" needs a number";
                        return false;
                    }
                    if (t.byte) spec.table_bytes.emplace_back(t.table, std::clamp(v.get<int>(), 0, 255));
                    else spec.table_floats.emplace_back(t.table, v.get<float>());
                }
                if (key == "top_gear") {
                    known = true;
                    if (!v.is_number_integer() || v.get<int>() < 1 || v.get<int>() > 4) {
                        error = "\"physics\" \"top_gear\" needs 1 to 4";
                        return false;
                    }
                    spec.top_gear = v.get<int>();
                }
                else if (key == "tire_curves") {
                    known = true;
                    std::string s = v.is_string() ? upper(v.get<std::string>()) : std::string();
                    if (s != "2049" && s != "BASE") {
                        error = "\"physics\" \"tire_curves\" is \"base\" or \"2049\"";
                        return false;
                    }
                    spec.tires_2049 = s == "2049";
                }
                if (!known) {
                    error = "\"physics\" has an unknown key \"" + key + "\"";
                    return false;
                }
            }
        }
        // Color schemes: whole liveries, each its own glb, picked by the car select's STYLE row (as a Dreamcast Rush
        // 2049 car's paint jobs) in place of MAIN and ACCENT COLOR.
        schemes.clear();
        if (j.contains("schemes")) {
            const auto& list = j["schemes"];
            if (!list.is_array() || list.empty() || list.size() > size_t(rush2::car2049::max_schemes)) {
                error = "\"schemes\" needs a list of 1 to " + std::to_string(rush2::car2049::max_schemes) + " .glb files";
                return false;
            }
            for (const auto& f : list) {
                std::string file = f.is_string() ? f.get<std::string>() : std::string();
                if (file.empty() || file.find_first_of("/\\") != std::string::npos) {
                    error = "\"schemes\" entries are .glb file names in rush2/new_cars/";
                    return false;
                }
                schemes.push_back(file);
            }
        }
        // The hood camera's hood (H0_L / H2_L, as a Rush 2049 car's HOOD): one glb per scheme, or one for the car.
        hoods.clear();
        if (j.contains("hoods")) {
            const auto& list = j["hoods"];
            size_t want = std::max<size_t>(1, schemes.size());
            if (!list.is_array() || list.size() != want) {
                error = "\"hoods\" needs a list of " + std::to_string(want) + " .glb files (one per scheme)";
                return false;
            }
            for (const auto& f : list) {
                std::string file = f.is_string() ? f.get<std::string>() : std::string();
                if (file.find_first_of("/\\") != std::string::npos) {
                    error = "\"hoods\" entries are .glb file names in rush2/new_cars/ (\"\" for none)";
                    return false;
                }
                hoods.push_back(file);
            }
        }
        return true;
    }

    // The box Rush 2 gives a car for hitting other cars (front, rear, half width, height in feet), from the mesh as
    // Rush 2's own relate to theirs: about a quarter foot inside the body at either end, at least 3 ft to a side,
    // 3 ft high.
    void box_from_model(const Model& model, float box[4]) {
        float lo_z = 0.0f, hi_z = 0.0f, half = 0.0f;
        for (const Part& p : model.parts) {
            if (p.hood) continue; // wider than the body
            for (const auto& v : p.pos) {
                lo_z = std::min(lo_z, v[2] / 16.0f);
                hi_z = std::max(hi_z, v[2] / 16.0f);
                half = std::max(half, std::fabs(v[0]) / 16.0f);
            }
        }
        box[0] = std::max(1.0f, hi_z - 0.3f);
        box[1] = std::max(1.0f, -lo_z - 0.25f);
        box[2] = std::max(3.0f, half);
        box[3] = 3.0f;
    }

    // The new cars of the enabled mods, each in the mod car slot it fills, into the plan.
    void load_new_cars(const std::vector<ModFile>& mods, Plan& plan) {
        std::vector<NewCarFile> files = new_car_files(mods);
        int slot = 0;
        for (const NewCarFile& f : files) {
            if (slot >= new_car_slots) {
                fprintf(stderr, "[newcar] %s in %s: every mod car slot is taken (%d)\n", f.id.c_str(),
                        mods[f.mod].mod_id.c_str(), new_car_slots);
                continue;
            }
            const std::string base = "rush2/new_cars/" + f.id;
            std::vector<uint8_t> text;
            std::vector<std::vector<uint8_t>> glbs, hood_glbs;
            std::vector<std::string> scheme_files, hood_files;
            rush2::car2049::ModCar spec;
            std::string error;
            if (!read_mod_file(mods[f.mod], base + ".json", text)) {
                error = "can't read " + f.id + ".json";
            }
            else if (read_new_car(text, spec, scheme_files, hood_files, error)) {
                if (scheme_files.empty()) scheme_files.push_back(f.id + ".glb");
                for (const std::string& file : scheme_files) {
                    glbs.emplace_back();
                    if (!read_mod_file(mods[f.mod], "rush2/new_cars/" + file, glbs.back())) {
                        error = "can't read " + file;
                        break;
                    }
                }
                for (const std::string& file : hood_files) {
                    hood_glbs.emplace_back();
                    if (!file.empty() && !read_mod_file(mods[f.mod], "rush2/new_cars/" + file, hood_glbs.back())) {
                        error = "can't read " + file;
                        break;
                    }
                }
                spec.schemes = int(glbs.size());
            }
            if (!error.empty()) {
                fprintf(stderr, "[newcar] %s in %s: %s\n", f.id.c_str(), mods[f.mod].mod_id.c_str(), error.c_str());
                continue;
            }
            int n = slot++;
            NewCarPlan c;
            c.n = n;
            c.id = f.id;
            c.mod_id = mods[f.mod].mod_id;
            c.hash = rush2::track_cache::hash(text.data(), text.size(), 0);
            for (const auto& g : glbs) c.hash = rush2::track_cache::hash(g.data(), g.size(), c.hash);
            for (const auto& g : hood_glbs) c.hash = rush2::track_cache::hash(g.data(), g.size(), c.hash ^ 0x400Dull);
            c.key = c.mod_id + "/" + f.id + "/" + std::to_string(c.hash);
            if (!convert("NC" + std::to_string(n + 1), "newcar_" + std::to_string(n + 1), uint32_t(64 + n), glbs, c.model,
                         c.car, c.stand_ins, error, true, hood_glbs)) {
                fprintf(stderr, "[newcar] %s in %s: %s\n", f.id.c_str(), mods[f.mod].mod_id.c_str(), error.c_str());
                slot--;
                continue;
            }
            if (!spec.has_box) {
                box_from_model(*c.model, spec.box);
                spec.has_box = true;
            }
            // The paints its textures take (texels 0 and 1 of a stand-in only name the ramps for rush2::derive), so the
            // car select shows no MAIN or ACCENT COLOR row that would change nothing.
            spec.paint_main = spec.paint_accent = false;
            for (const StandIn& s : c.stand_ins) {
                for (size_t i = 2; i < s.texels.size(); i++) {
                    spec.paint_main |= s.texels[i] >= main_first && s.texels[i] <= main_last;
                    spec.paint_accent |= s.texels[i] >= accent_first && s.texels[i] <= accent_last;
                }
            }
            c.spec = spec;
            plan.new_cars.push_back(std::move(c));
        }
        plan.slots_used = slot;
    }

    // Each Rush 2 car's replacement (rush2/cars/<prefix>.glb, the first mod in order that has it), into the plan.
    void load_replacements(const std::vector<ModFile>& mods, Plan& plan) {
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
            ReplacementPlan& r = plan.replacements[t];
            if (from == nullptr) continue;
            std::vector<uint8_t> hood;
            read_mod_file(*from, std::string("rush2/cars/") + prefixes[t] + "_HOOD.glb", hood);
            r.mod_id = from->mod_id;
            r.hash = rush2::track_cache::hash(glb.data(), glb.size());
            if (!hood.empty()) r.hash = rush2::track_cache::hash(hood.data(), hood.size(), r.hash ^ 0x400Dull);
            std::string error;
            if (!convert(prefixes[t], std::string("carmod_") + prefixes[t], uint32_t(t), { glb }, r.model, r.car,
                         r.stand_ins, error, true, { hood })) {
                fprintf(stderr, "[carmod] %s in %s: %s\n", name.c_str(), from->mod_id.c_str(), error.c_str());
                r.state = ReplacementPlan::failed;
                continue;
            }
            r.state = ReplacementPlan::ok;
        }
    }

    // The plan's key: the enabled mods (ids, on or off, files) and when each mod's file or folder last changed.
    std::string plan_key(const std::vector<ModFile>& mods, const std::string& signature) {
        std::string key = signature;
        for (const ModFile& m : mods) {
            std::error_code ec;
            auto time = std::filesystem::last_write_time(m.path, ec);
            key += std::to_string(ec ? 0 : time.time_since_epoch().count()) + ";";
        }
        return key;
    }

    Plan make_plan(const std::vector<ModFile>& mods, const std::string& key) {
        ZipBatch batch;
        Plan plan;
        plan.key = key;
        load_models(mods, plan);
        load_liveries(mods, plan);
        load_new_cars(mods, plan);
        load_replacements(mods, plan);
        return plan;
    }

    // The plan handed from the background (rush2::car_mods::prepare) to apply: `planning` while one is made, by the
    // background or by apply itself, so the two never make the same plan at once.
    std::mutex plan_mutex;
    std::condition_variable plan_done;
    bool planning = false;
    std::optional<Plan> ready;

    // The plan for these mods: the background's if it made it (waiting while one is being made), else made now.
    Plan obtain_plan(const std::vector<ModFile>& mods, const std::string& key) {
        {
            std::unique_lock lock{ plan_mutex };
            plan_done.wait(lock, [] { return !planning; });
            if (ready && ready->key == key) {
                Plan plan = std::move(*ready);
                ready.reset();
                return plan;
            }
            planning = true;
        }
        Plan plan = make_plan(mods, key);
        std::lock_guard lock{ plan_mutex };
        planning = false;
        plan_done.notify_all();
        return plan;
    }

    // Makes the plan for the enabled mods as they are now and leaves it for apply, unless one is being made or is
    // waiting already for them.
    void prepare_plan() {
        if (!load_car_palette()) return;
        std::string signature;
        std::vector<ModFile> mods = enabled_mods(signature);
        std::string key = plan_key(mods, signature);
        {
            std::lock_guard lock{ plan_mutex };
            if (planning || (ready && ready->key == key)) return;
            planning = true;
        }
        Plan plan = make_plan(mods, key);
        std::lock_guard lock{ plan_mutex };
        ready = std::move(plan);
        planning = false;
        plan_done.notify_all();
    }

    // Installs a plan: the model mods' and liveries' patcher, the new cars in their slots and each car's replacement,
    // skipping what is installed already from the same files.
    void install(uint8_t* rdram, Plan& plan) {
        bool patched = false;
        if (plan.model_signature != model_signature) {
            model_signature = plan.model_signature;
            model_mods = std::move(plan.models);
            printf("[modelmod] %zu model replacements\n", model_mods.size());
            patched = true;
        }
        if (plan.livery_signature != livery_signature) {
            livery_signature = plan.livery_signature;
            liveries = std::move(plan.liveries);
            for (int t = 0; t < types_all; t++) livery_counts[t] = plan.livery_counts[t];
            // The bodies are made again as the cars load with the new liveries.
            {
                std::lock_guard lock{ style_mutex };
                shown.clear();
            }
            {
                std::lock_guard lock{ livery_tables_mutex };
                livery_bodies.clear();
                rush2::car2049::clear_livery_bodies();
            }
            size_t total = 0;
            for (const auto& [index, list] : liveries) total += list.size();
            printf("[livery] %zu liveries for %zu cars\n", total, liveries.size());
            patched = true;
        }
        if (patched) {
            rush2::assets::set_patcher(rdram, model_mods.empty() && liveries.empty() ? rush2::assets::Patcher()
                                                                                       : rush2::assets::Patcher(patch_container));
        }
        bool changed = patched;
        for (NewCarPlan& c : plan.new_cars) {
            int n = c.n;
            if (applied_new_key[n] == c.key) continue;
            std::map<uint32_t, std::vector<uint8_t>> scuffed;
            size_t triangles = 0;
            for (const Part& p : c.model->parts) triangles += p.tris.size() / 3;
            for (const StandIn& s : c.stand_ins) {
                if (s.scuffed != s.texels) scuffed[s.offset] = s.scuffed; // unpainted: nothing to scuff
            }
            size_t car_bytes = c.car.size();
            rush2::car2049::set_mod_car(rdram, n, std::move(c.car), "NC" + std::to_string(n + 1), scuffed, c.spec);
            printf("[newcar] %s (%s) from %s as type %d: %zu triangles, %zu textures, %d scheme(s), %zu KB, based on %s\n",
                   c.spec.name.c_str(), c.id.c_str(), c.mod_id.c_str(), rush2::car_types::first_mod + n,
                   triangles, c.stand_ins.size(), c.spec.schemes, car_bytes / 1024, prefixes[c.spec.base]);
            applied_new_key[n] = c.key;
            applied_new[n].mod_id = c.mod_id;
            applied_new[n].glb_hash = c.hash;
            applied_new[n].stand_ins = std::move(c.stand_ins);
            applied_new[n].model = c.model;
            changed = true;
        }
        for (int n = plan.slots_used; n < new_car_slots; n++) {
            if (applied_new_key[n].empty()) continue;
            rush2::car2049::clear_mod_car(rdram, n);
            printf("[newcar] mod car slot %d emptied\n", n + 1);
            applied_new_key[n].clear();
            applied_new[n] = Applied{};
            changed = true;
        }
        for (int t = 0; t < types; t++) {
            ReplacementPlan& r = plan.replacements[t];
            Applied& a = applied[t];
            if (r.state == ReplacementPlan::none) {
                if (a.model) {
                    rush2::car2049::clear_body_car(rdram, t, own[t]);
                    printf("[carmod] %s: back to its own car\n", prefixes[t]);
                    a = Applied{};
                    changed = true;
                }
                continue;
            }
            if (r.state == ReplacementPlan::failed || (a.model && a.glb_hash == r.hash && a.mod_id == r.mod_id)) continue;
            std::shared_ptr<Model> model = r.model;
            std::vector<uint8_t> car = std::move(r.car);
            std::vector<StandIn> stand_ins = std::move(r.stand_ins);
            if (!has_own[t]) {
                std::vector<rush2::assets::Span> spans;
                own[t].clear();
                rush2::assets::replacement(first_car_asset + t, own[t], spans);
                has_own[t] = true;
            }
            std::map<uint32_t, std::vector<uint8_t>> scuffed;
            size_t triangles = 0;
            for (const Part& p : model->parts) triangles += p.tris.size() / 3;
            for (const StandIn& s : stand_ins) {
                if (s.scuffed != s.texels) scuffed[s.offset] = s.scuffed; // unpainted: nothing to scuff
            }
            rush2::car2049::set_body_car(rdram, t, std::move(car), prefixes[t], scuffed);
            printf("[carmod] %s from %s: %zu triangles, %zu textures\n", prefixes[t], r.mod_id.c_str(), triangles,
                   stand_ins.size());
            a.mod_id = r.mod_id;
            a.glb_hash = r.hash;
            a.stand_ins = std::move(stand_ins);
            a.model = model;
            changed = true;
        }
        if (changed) publish_textures();
        fflush(stdout);
    }

    // At boot: the plan the background made for these mods (or made now), installed.
    void apply(uint8_t* rdram) {
        std::string signature;
        std::vector<ModFile> mods = enabled_mods(signature);
        last_signature = signature;
        Plan plan = obtain_plan(mods, plan_key(mods, signature));
        install(rdram, plan);
    }
}

void rush2::car_mods::prepare() {
    prepare_plan();
}

std::vector<rush2::mod_files::ModFile> rush2::mod_files::enabled_mods(std::string& signature) {
    std::vector<ModFile> out;
    for (const recomp::mods::ModDetails& mod : recomp::mods::get_all_mod_details("rush2")) {
        bool on = recomp::mods::is_mod_enabled(mod.mod_id);
        // The mod's file too: a mod reinstalled as an .rtz in place of its folder (src/arcade2049) keeps its id.
        std::filesystem::path file = recomp::mods::get_mod_filename(mod.mod_id);
        signature += mod.mod_id + (on ? "+" : "-") + rush2::upscale::path_utf8(file) + ";";
        if (on) out.push_back({ mod.mod_id, file });
    }
    return out;
}

bool rush2::mod_files::read_mod_file(const ModFile& mod, const std::string& name, std::vector<uint8_t>& out) {
    if (open_zips != nullptr) {
        rush2::upscale::ZipReader& zip = zip_of(mod.path);
        if (zip.is_open()) return zip.read(name, out);
        out = rush2::upscale::read_file(mod.path / std::filesystem::u8path(name));
        return !out.empty();
    }
    rush2::upscale::ZipReader zip(mod.path);
    if (zip.is_open()) return zip.read(name, out);
    out = rush2::upscale::read_file(mod.path / std::filesystem::u8path(name));
    return !out.empty();
}

void rush2::car_mods::init(uint8_t* rdram) {
    initialized = load_car_palette();
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
    // Mods turned on or off (or reinstalled) since: their plan is made on a background thread and installed by a later
    // check once it is ready, so the menus never wait on the mods' files.
    static bool want = false;
    std::string signature;
    std::vector<ModFile> mods = enabled_mods(signature);
    if (signature != last_signature || reload_requested.exchange(false)) {
        want = true;
        std::thread([] { prepare_plan(); }).detach();
    }
    if (!want) return;
    std::string key = plan_key(mods, signature);
    std::optional<Plan> plan;
    {
        std::lock_guard lock{ plan_mutex };
        if (planning) return;
        if (ready && ready->key == key) {
            plan = std::move(ready);
            ready.reset();
        }
    }
    if (!plan) {
        std::thread([] { prepare_plan(); }).detach();   // the mods changed again while it was made
        return;
    }
    want = false;
    last_signature = signature;
    install(rdram, *plan);
}

void rush2::car_mods::reload() {
    reload_requested = true;
}
