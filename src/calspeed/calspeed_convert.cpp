// California Speed tracks converted for a Rush 2 slot, and their track select art (docs/calspeed_research.md).
//
// California Speed's N64 version runs on the Rush 2 engine, so its track files are Rush 2 files already: geometry
// (asset 62 + t), placement (78 + t), collision (94 + t) and AI path (110 + t) load as they are. The conversion only
// does what the host slot needs:
// - Geometry: the track's container merged with California Speed's shared object containers its placement uses
//   (cones, lamps, trees, buildings: assets 26-38), plus the models and textures Rush 2 looks up unchecked by name
//   (<prefix>FINISH, <prefix>FINISHB, CHKPNT, FINISH: empty, as for the Rush 2049 tracks). Every model's behaviour id is
//   cleared: California Speed numbers its own "hit" objects (1 tree, 2 light, 3 lamp, 4 cone, 5 plum tree, 6 window,
//   7 bomb, 8 building, 9 stealth bomber, 10 helicopter), which mean other behaviours in Rush 2's table.
// - Placement: the tree's name becomes the host's prefix. California Speed's hit objects animate through frames I1..In
//   where Rush 2's breakables fly apart, so they become Rush 2 breakable class records drawn with their first frame,
//   as the SF Rush tracks' do (src/rush1/track1.cpp redirects the lookups): trees and plum trees TREEHIT, cones CONE1,
//   lamps and lights CURVEHIT (a metal pole). Windows and buildings stay as they are, unbreakable; the helicopter and
//   the stealth bomber (which fly in California Speed) draw nothing.
// - Path: California Speed's own (one per track: there are no backward races), with Rush 2's loader working out the
//   checkpoint times from the AI lanes as for its own tracks. The path's stored times are those same lane times.
// - Visibility (0x800C0680, 16 bytes per region, the tracks' tables back to back, counts at 0x800C4C80), fog colour
//   (0x80094CB4), cloud scroll (0x80094C2C) and sky parallax (0x80094C6C) from California Speed's per-track tables,
//   which have Rush 2's layout.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <vector>

#include "calspeed.h"
#include "texture_upscale.h"
#include "track2049.h"
#include "track2049_convert.h"
#include "track_cache.h"

namespace {
    using Bytes = std::vector<uint8_t>;

    constexpr int geometry_asset = 62, placement_asset = 78, collision_asset = 94, path_asset = 110;
    constexpr int banner_asset = 3;      // Track select art: the 144 x 23 track name banners.
    constexpr int landmark_asset = 6;    // The track select's landmark models.
    constexpr int first_object_asset = 26, last_object_asset = 38;   // Shared hit objects.

    constexpr uint32_t cloud_scroll = 0x80094C2C;   // f32 per track.
    constexpr uint32_t sky_parallax = 0x80094C6C;   // f32 per track.
    constexpr uint32_t fog_colours = 0x80094CB4;    // RGB per track.
    constexpr uint32_t pvs_counts = 0x800C4C80;     // u8 per track.
    constexpr uint32_t pvs_tables = 0x800C0680;     // 16 bytes per region, the tracks' tables back to back.
    constexpr int max_regions = 128;                // The host's visibility table (rush2::track2049) holds 128.

    // Track select names of the banners (asset 3) and landmarks (asset 6), per track.
    constexpr const char* banner_names[rush2::calspeed::track_count] = { "MONTEREY", "SV", "HWY1", "SANFRANSICO", "CV",
        "SC", "SD", "CLOVER", "LA", "SHASTA", "YOSEMITE", "MOJAVE", "FUJI", "OVAL", "CALIFORNIA", "SANAND" };
    constexpr const char* landmark_names[rush2::calspeed::track_count] = { "MONTEREYNICHIME", "SILLYVALLEYNICH",
        "HWY1NICHIMEN", "SANFRANNICHIMEN", "CENTRALNICHIMEN", "SANTACRUZNICHIM", "SANDIEGONICHIME", "CLOVERNICHIMEN",
        "LANICHIMEN", "SHASTANICHIMEN", "YOSEMITENICHIME", "MOJAVENICHIMEN", "FUJINICHIMEN", "OVALNICHIMEN",
        "CALIFORNIANICHI", "SANANDNICHIMEN" };
    // Radius x scale of a landmark on the carousel. Rush 2's dioramas are flat maps at about 34 (VEGASTRACK 42.9 x 0.8,
    // LATRACK 48.8 x 0.7); a landmark is as deep as it is wide, so at that size its near side reaches the camera.
    constexpr float diorama_size = 17.0f;

    constexpr int logo_w = 128, logo_h = 32;

    struct ConvertError {
        std::string message;
    };
    [[noreturn]] void fail(const std::string& message) {
        throw ConvertError{ message };
    }

    uint32_t u32(const Bytes& d, size_t o) {
        if (o + 4 > d.size()) fail("read past the end of a file");
        return (uint32_t(d[o]) << 24) | (uint32_t(d[o + 1]) << 16) | (uint32_t(d[o + 2]) << 8) | d[o + 3];
    }
    uint16_t u16(const Bytes& d, size_t o) {
        if (o + 2 > d.size()) fail("read past the end of a file");
        return uint16_t((d[o] << 8) | d[o + 1]);
    }
    void put32(Bytes& d, size_t o, uint32_t v) {
        d[o] = uint8_t(v >> 24); d[o + 1] = uint8_t(v >> 16); d[o + 2] = uint8_t(v >> 8); d[o + 3] = uint8_t(v);
    }
    void add32(Bytes& d, uint32_t v) {
        d.push_back(uint8_t(v >> 24)); d.push_back(uint8_t(v >> 16)); d.push_back(uint8_t(v >> 8)); d.push_back(uint8_t(v));
    }
    std::string name_at(const Bytes& d, size_t o) {
        std::string s;
        for (size_t i = 0; i < 16 && o + i < d.size() && d[o + i] != 0; i++) s.push_back(char(d[o + i]));
        return s;
    }
    void set_name(Bytes& d, size_t o, const std::string& s) {
        for (size_t i = 0; i < 16; i++) d[o + i] = i < s.size() && i < 15 ? uint8_t(s[i]) : 0;
    }
    bool starts_with(const std::string& s, const char* p) {
        return s.rfind(p, 0) == 0;
    }

    Bytes asset(const Bytes& rom, int index) {
        Bytes out;
        if (!rush2::calspeed::read_asset(rom, index, out)) fail("can't read California Speed asset " + std::to_string(index));
        return out;
    }

    uint8_t main_byte(const Bytes& rom, uint32_t vram) {
        return uint8_t(rush2::calspeed::main_word(rom, vram & ~3u) >> (24 - 8 * (vram & 3)));
    }

    // ------------------------------------------------------------------------------------------------------------
    // Placement

    // What a California Speed placement record becomes in Rush 2.
    struct Mapped {
        std::string record;    // The record's new name ("" = keep).
        std::string model;     // The model it draws, for a breakable class record ("" = none).
        uint16_t kind = 0;     // The model's behaviour id: that of the Rush 2 model the class draws.
    };

    // Rush 2 breakable classes (func_800815DC's table 0x800C5400) for California Speed's hit objects, by the record's
    // name. Behaviour ids are those of the class's Rush 2 model (TREEHIT1O1 5, CONE1O1 2, CURVEHITO1 5).
    Mapped map_record(const std::string& name) {
        if (starts_with(name, "TREEHIT")) return { name, name, 5 };     // already a TREEHIT record
        if (name == "PLUMHIT1I1") return { "TREEHITPLUM1", name, 5 };
        if (name == "CON17HIT1I1") return { "CONE1CS1", name, 2 };
        if (name == "LAMPHIT1I1") return { "CURVEHITLAMP1", name, 5 };
        if (name == "LAMPHIT2I1") return { "CURVEHITLAMP2", name, 5 };
        if (name == "LIGHTHIT1I1") return { "CURVEHITLIGHT1", name, 5 };
        // WINDOWn... would be Rush 2 WINDOW records, whose pieces aren't loaded on this slot: plain models instead.
        if (starts_with(name, "WINDOW")) return { "CS" + name, "", 0 };
        if (name == "HELII1" || name == "STEALTHI1") return { "CSNONE", "", 0 };
        return {};
    }

    // Rush 2 placement file (docs/rush2049_research/placement.md section 1): u32 count, count x {u32 offset, name[16]},
    // then 0x64-byte records. Renames the tree to `prefix` and the records map_record maps.
    Bytes convert_placement(const Bytes& src, const std::string& prefix, std::map<std::string, std::string>& record_models,
                            std::map<std::string, std::string>& renamed_models, std::map<std::string, uint16_t>& kinds,
                            std::set<std::string>& used) {
        Bytes out = src;
        uint32_t count = u32(out, 0);
        if (count == 0 || count > 4 || 4 + count * 0x14 > out.size()) fail("bad placement directory");
        uint32_t first = u32(out, 4);
        set_name(out, 8, prefix);
        if (first > out.size() || (out.size() - first) % 0x64 != 0) fail("bad placement records");
        for (size_t o = first; o + 0x64 <= out.size(); o += 0x64) {
            std::string name = name_at(out, o);
            Mapped m = map_record(name);
            std::string drawn = name;
            if (!m.record.empty()) {
                set_name(out, o, m.record);
                if (!m.model.empty()) {
                    record_models[m.record] = m.model;
                    kinds[m.model] = m.kind;
                }
                else {
                    drawn = m.record;
                    if (m.record != "CSNONE") renamed_models[name] = m.record;
                }
            }
            used.insert(m.model.empty() ? drawn : m.model);
        }
        return out;
    }

    // ------------------------------------------------------------------------------------------------------------
    // Track select landmark: one model of a Rush 2 container lifted out with what it draws

    struct Container {
        const Bytes& d;
        uint32_t h[10];
        explicit Container(const Bytes& data) : d(data) {
            if (d.size() < 0x28) fail("container too small");
            for (int i = 0; i < 10; i++) h[i] = u32(d, i * 4);
        }
        // LOD 0 list and radius (f32 bits) of model `name`.
        bool find(const std::string& name, uint32_t& list, float& radius) const {
            for (uint32_t i = 0; i < h[4]; i++) {
                if (name_at(d, h[1] + i * 0x18) == name) {
                    list = u32(d, h[0] + i * 0x34 + 4 + 8);
                    uint32_t bits = u32(d, h[1] + i * 0x18 + 16);
                    memcpy(&radius, &bits, 4);
                    return list != 0;
                }
            }
            return false;
        }
        bool texture_load(uint32_t a) const { return a >= h[7] && a < h[8]; }
    };

    // Bytes a SETTIMG at `o` (in list d) loads, from the load command after it: LOADBLOCK, LOADTILE or LOADTLUT.
    uint32_t settimg_size(const Bytes& d, size_t o) {
        uint32_t w0 = u32(d, o);
        uint32_t siz = (w0 >> 19) & 3, width = (w0 & 0xFFF) + 1;
        for (size_t p = o + 8; p + 8 <= d.size() && p < o + 8 * 12; p += 8) {
            uint32_t c0 = u32(d, p), c1 = u32(d, p + 4);
            switch (c0 >> 24) {
                case 0xF3: return (((c1 >> 12) & 0xFFF) + 1) * (1u << siz) / 2;                 // LOADBLOCK
                case 0xF0: return (((c1 >> 14) & 0x3FF) + 1) * 2;                               // LOADTLUT
                case 0xF4: return ((c1 & 0xFFF) / 4 + 1) * width * (1u << siz) / 2;              // LOADTILE
                case 0xFD: case 0xDF: return 0;
            }
        }
        return 0;
    }

    // The model list copied flat: sub-lists that aren't texture-load lists inlined, vertices and texels copied into the
    // blob before it, texture-load lists into `loads`.
    struct Lifted {
        Bytes data, list, loads;
        std::vector<uint32_t> list_pointers;    // in list, pointing into data
        std::vector<uint32_t> list_calls;       // in list, pointing into loads
        std::vector<uint32_t> load_pointers;    // in loads, pointing into data
        std::map<std::pair<uint32_t, uint32_t>, uint32_t> copied;   // (source, size) -> offset in data
        std::map<uint32_t, uint32_t> load_lists;                    // source list -> offset in loads
        std::set<std::pair<uint32_t, uint32_t>> vertices;           // (offset in data, size) of each vertex block
    };

    uint32_t copy_data(const Container& c, Lifted& l, uint32_t src, uint32_t size) {
        if (size == 0 || src + size > c.d.size()) fail("data outside the container");
        auto it = l.copied.find({ src, size });
        if (it != l.copied.end()) return it->second;
        while (l.data.size() % 8) l.data.push_back(0);
        uint32_t at = uint32_t(l.data.size());
        l.data.insert(l.data.end(), c.d.begin() + src, c.d.begin() + src + size);
        l.copied[{ src, size }] = at;
        return at;
    }

    uint32_t copy_loads(const Container& c, Lifted& l, uint32_t src) {
        auto it = l.load_lists.find(src);
        if (it != l.load_lists.end()) return it->second;
        uint32_t at = uint32_t(l.loads.size());
        l.load_lists[src] = at;
        for (size_t o = src; ; o += 8) {
            uint32_t w0 = u32(c.d, o), w1 = u32(c.d, o + 4);
            uint8_t op = uint8_t(w0 >> 24);
            if (op == 0xE1 || op == 0x04 || op == 0xDE || op == 0x01) fail("unexpected command in a texture-load list");
            if (op == 0xFD) {
                uint32_t to = copy_data(c, l, w1 & 0xFFFFFF, settimg_size(c.d, o));
                l.load_pointers.push_back(uint32_t(l.loads.size() + 4));
                w1 = (w1 & 0xFF000000) | to;
            }
            add32(l.loads, w0);
            add32(l.loads, w1);
            if (op == 0xDF) break;
        }
        return at;
    }

    void lift_list(const Container& c, Lifted& l, uint32_t src, int depth) {
        if (depth > 8) fail("display lists nested too deep");
        for (size_t o = src; ; o += 8) {
            uint32_t w0 = u32(c.d, o), w1 = u32(c.d, o + 4);
            uint8_t op = uint8_t(w0 >> 24);
            if (op == 0xDF) return;
            if (op == 0xDE) {
                uint32_t t = w1 & 0xFFFFFF;
                bool branch = ((w0 >> 16) & 0xFF) == 1;
                if (c.texture_load(t)) {
                    l.list_calls.push_back(uint32_t(l.list.size() + 4));
                    add32(l.list, 0xDE000000);
                    add32(l.list, copy_loads(c, l, t));
                }
                else {
                    lift_list(c, l, t, depth + 1);
                }
                if (branch) return;
                continue;
            }
            if (op == 0x01 || op == 0xFD) {
                uint32_t size = op == 0x01 ? ((w0 >> 12) & 0xFF) * 16 : settimg_size(c.d, o);
                uint32_t to = copy_data(c, l, w1 & 0xFFFFFF, size);
                if (op == 0x01) l.vertices.insert({ to, size });
                l.list_pointers.push_back(uint32_t(l.list.size() + 4));
                w1 = (w1 & 0xFF000000) | to;
            }
            else if (op == 0xDA || op == 0xDC || op == 0x04 || op == 0xE1 || (op == 0xE0 && ((w0 >> 16) & 0xFF) == 1)) {
                fail("unsupported command in a landmark's list");
            }
            add32(l.list, w0);
            add32(l.list, w1);
        }
    }

    bool lift_model(const Bytes& container, const std::string& name, rush2::track2049::MenuModel& out) {
        try {
            Container c(container);
            uint32_t list;
            float radius;
            if (!c.find(name, list, radius)) return false;
            Lifted l;
            lift_list(c, l, list, 0);
            // California Speed shows a landmark as a relief in a niche, its surface facing -x; Rush 2's
            // carousel looks down on a diorama standing on y. Turned a quarter about z so -x is up: (x, y, z) -> (y, -x, z).
            // The vertices are unlit (no normals), so only positions turn.
            // The relocator walks each G_VTX block's vertex chain (bit 15 of +6, mirror_vertex_chain_80077810, marking
            // each with 0x4000 and flipping s when mirrored); California Speed chains a model's vertices on past each
            // block, which here would run into the texels copied after it, so each block's last vertex ends its chain.
            for (auto [at, size] : l.vertices) {
                if (size >= 16) l.data[at + size - 10] &= 0x7F;
            }
            std::set<uint32_t> turned;
            for (auto [at, size] : l.vertices) {
                for (uint32_t v = at; v + 16 <= at + size; v += 16) {
                    if (!turned.insert(v).second) continue;
                    uint16_t x = u16(l.data, v), y = u16(l.data, v + 2);
                    uint16_t nx = uint16_t(-int16_t(x));
                    l.data[v] = uint8_t(y >> 8);
                    l.data[v + 1] = uint8_t(y);
                    l.data[v + 2] = uint8_t(nx >> 8);
                    l.data[v + 3] = uint8_t(nx);
                }
            }
            add32(l.list, 0xDF000000);
            add32(l.list, 0);
            while (l.data.size() % 8) l.data.push_back(0);
            uint32_t base = uint32_t(l.data.size());
            out.blob = l.data;
            out.blob.insert(out.blob.end(), l.list.begin(), l.list.end());
            out.list = base;
            out.pointers.clear();
            for (uint32_t p : l.list_pointers) out.pointers.push_back(base + p);
            out.load_calls.clear();
            for (uint32_t p : l.list_calls) out.load_calls.push_back(base + p);
            out.loads = l.loads;
            out.load_pointers = l.load_pointers;
            out.radius = radius;
            return true;
        }
        catch (const ConvertError& e) {
            printf("[CalSpeed] Landmark %s: %s\n", name.c_str(), e.message.c_str());
            return false;
        }
    }

    // ------------------------------------------------------------------------------------------------------------
    // Logos: the track select banners (CI4 144 x 23, rows bottom-up) as Rush 2 logos (CI8 128 x 32, rows bottom-up)

    bool banner_rgba(const Bytes& container, const std::string& name, int& w, int& h, std::vector<std::array<uint8_t, 4>>& rgba) {
        Container c(container);
        for (uint32_t i = 0; i < c.h[5]; i++) {
            size_t r = c.h[2] + i * 0x20;
            if (name_at(c.d, r) != name) continue;
            w = u16(c.d, r + 16);
            h = u16(c.d, r + 18);
            uint8_t siz = c.d[r + 20], fmt = c.d[r + 21];
            int16_t pal = int16_t(u16(c.d, r + 22));
            uint32_t texels = u32(c.d, r + 24);
            if (siz != 0 || fmt != 2 || pal < 0 || uint32_t(pal) >= c.h[6] || texels + uint32_t(w * h / 2) > c.d.size()) return false;
            uint32_t colors = u32(c.d, c.h[3] + uint32_t(pal) * 0x18 + 20);
            rgba.assign(size_t(w * h), {});
            for (int p = 0; p < w * h; p++) {
                uint8_t b = c.d[texels + p / 2];
                uint16_t v = u16(c.d, colors + 2 * ((p & 1) ? (b & 15) : (b >> 4)));
                rgba[size_t(p)] = { uint8_t(((v >> 11) & 31) * 255 / 31), uint8_t(((v >> 6) & 31) * 255 / 31),
                                    uint8_t(((v >> 1) & 31) * 255 / 31), uint8_t((v & 1) ? 255 : 0) };
            }
            return true;
        }
        return false;
    }

    // The banner shrunk to 128 wide (box filter over premultiplied colour), kept in proportion and centred in 32 rows.
    bool build_logo(const Bytes& banners, int t, rush2::track2049::MenuLogo& logo, rush2::upscale::Image& image) {
        int w, h;
        std::vector<std::array<uint8_t, 4>> src;
        if (!banner_rgba(banners, banner_names[t], w, h, src)) return false;
        int dh = std::max(1, std::min(logo_h, (int)std::lround(h * double(logo_w) / w)));
        int top = (logo_h - dh) / 2;
        std::vector<std::array<uint8_t, 4>> out(size_t(logo_w * logo_h), std::array<uint8_t, 4>{ 0, 0, 0, 0 });
        for (int y = 0; y < dh; y++) {
            double y0 = double(y) * h / dh, y1 = double(y + 1) * h / dh;
            for (int x = 0; x < logo_w; x++) {
                double x0 = double(x) * w / logo_w, x1 = double(x + 1) * w / logo_w;
                double acc[4] = {}, area = 0;
                for (int sy = int(y0); sy < h && sy < y1; sy++) {
                    double fy = std::min<double>(sy + 1, y1) - std::max<double>(sy, y0);
                    for (int sx = int(x0); sx < w && sx < x1; sx++) {
                        double f = fy * (std::min<double>(sx + 1, x1) - std::max<double>(sx, x0));
                        const auto& p = src[size_t(sy * w + sx)];
                        double a = p[3] / 255.0;
                        for (int k = 0; k < 3; k++) acc[k] += p[k] * a * f;
                        acc[3] += a * f;
                        area += f;
                    }
                }
                auto& q = out[size_t((top + y) * logo_w + x)];
                if (area > 0 && acc[3] > 0) {
                    for (int k = 0; k < 3; k++) q[k] = uint8_t(std::clamp(acc[k] / acc[3], 0.0, 255.0));
                    q[3] = uint8_t(std::clamp(acc[3] / area * 255.0, 0.0, 255.0));
                }
            }
        }
        std::array<uint16_t, 256> palette{};
        rush2::track2049::quantize_ci8(out, logo.texels, palette);
        logo.palette.clear();
        for (uint16_t c : palette) {
            logo.palette.push_back(uint8_t(c >> 8));
            logo.palette.push_back(uint8_t(c));
        }
        logo.name = rush2::calspeed::menu_logo_name(t);
        image.width = logo_w;
        image.height = logo_h;
        image.rgba.clear();
        for (const auto& p : out) image.rgba.insert(image.rgba.end(), p.begin(), p.end());
        return logo.texels.size() == size_t(logo_w * logo_h);
    }

    // ------------------------------------------------------------------------------------------------------------
    // Disk cache blobs (rush2::track_cache)

    struct Writer {
        Bytes b;
        void word(uint32_t v) { add32(b, v); }
        void bytes(const Bytes& v) { word(uint32_t(v.size())); b.insert(b.end(), v.begin(), v.end()); }
        void str(const std::string& s) { bytes(Bytes(s.begin(), s.end())); }
        void words(const std::vector<uint32_t>& v) { word(uint32_t(v.size())); for (uint32_t x : v) word(x); }
        void map(const std::map<std::string, std::string>& m) {
            word(uint32_t(m.size()));
            for (const auto& [k, v] : m) { str(k); str(v); }
        }
    };
    struct Reader {
        const Bytes& b;
        size_t o = 0;
        bool ok = true;
        uint32_t word() {
            if (o + 4 > b.size()) { ok = false; return 0; }
            uint32_t v = u32(b, o);
            o += 4;
            return v;
        }
        Bytes bytes() {
            uint32_t n = word();
            if (!ok || o + n > b.size()) { ok = false; return {}; }
            Bytes v(b.begin() + o, b.begin() + o + n);
            o += n;
            return v;
        }
        std::string str() { Bytes v = bytes(); return std::string(v.begin(), v.end()); }
        std::vector<uint32_t> words() {
            uint32_t n = word();
            std::vector<uint32_t> v;
            for (uint32_t i = 0; ok && i < n; i++) v.push_back(word());
            return v;
        }
        std::map<std::string, std::string> map() {
            uint32_t n = word();
            std::map<std::string, std::string> m;
            for (uint32_t i = 0; ok && i < n; i++) { std::string k = str(); m[k] = str(); }
            return m;
        }
    };

    // Rush 2 lines the start grid up behind the spine point where checkpoint 0 crosses it, 60 ft deep (slot table
    // 0x800CC2BC, car_place_by_case_8006DE20 case 0; docs/rush2049_research/race.md#4-how-the-race-is-defined). On a
    // loop that runs back over the end of the lap; a point-to-point track's road starts only a few feet behind its
    // first spine point, and on Monterey, Silicon Valley, Highway 1, San Francisco, Central Valley and Yosemite
    // checkpoint 0 crosses within 50 ft of it, so the back rows (the player's) start off the road and fall. California
    // Speed's own grid isn't traced; checkpoint 0 is moved forward to the first spine stretch at least
    // `grid_depth` ft along, which puts every slot on the road on every track (checked against each track's collision).
    // Its crossings are worked out at load from its position and direction (func_80092BC4), so nothing else changes.
    constexpr double grid_depth = 75.0;

    constexpr size_t route = 0x32C, checkpoints = 0xC;

    void move_start(Bytes& path) {
        uint32_t spine = u16(path, route), branches = path.at(route + 8);
        size_t points = route + 16 + branches * 16 + 2;
        if (spine < 3 || u16(path, 8) == 0) return;
        auto at = [&](uint32_t i, int k) { return double(int16_t(u16(path, points + i * 6 + size_t(k) * 2))); };
        auto f32 = [&](size_t o) { uint32_t b = u32(path, o); float v; memcpy(&v, &b, 4); return double(v); };
        auto put_f32 = [&](size_t o, double v) { float x = float(v); uint32_t b; memcpy(&b, &x, 4); put32(path, o, b); };
        // The first spine point past checkpoint 0's plane (dir points back against the race).
        double cx = f32(checkpoints), cz = f32(checkpoints + 8), dx = f32(checkpoints + 0xC), dz = f32(checkpoints + 0x14);
        uint32_t cross = 0;
        while (cross < spine && (at(cross, 0) - cx) * dx + (at(cross, 2) - cz) * dz >= 0) cross++;
        double dist = 0.0;
        for (uint32_t i = 1; i < spine; i++) {
            dist += std::hypot(at(i, 0) - at(i - 1, 0), at(i, 2) - at(i - 1, 2));
            if (dist < grid_depth) continue;
            if (i <= cross) return;
            double ex = at(i - 1, 0) - at(i, 0), ez = at(i - 1, 2) - at(i, 2), len = std::hypot(ex, ez);
            if (len < 1.0) return;
            for (int k = 0; k < 3; k++) put_f32(checkpoints + size_t(k) * 4, (at(i - 1, k) + at(i, k)) / 2);
            put_f32(checkpoints + 0xC, ex / len);
            put_f32(checkpoints + 0x10, 0.0);
            put_f32(checkpoints + 0x14, ez / len);
            return;
        }
    }

    // California Speed numbers its surface types (POLY flags & 0xF, docs/rush2049_research/collision.md section 5) its
    // own way. Over its 16 tracks: 0-3 are ground, each of 1-3 with its own wheel material (1, 2, 3; 3 is 5745 flat
    // polygons on the verges), and 4-9 are walls (every polygon of 4, 7, 8 and 9 is vertical; its wall query at
    // 0x800AECC4 takes 5-9). In Rush 2 the type drives the physics (0 road, 1 dirt, 2 grass), 3 and 7 wreck a wheel
    // that touches them, 4 sinks the car, and only 5 and 6 are walls to the body probe: its cars wrecked on the verges
    // and drove through the 4, 7, 8 and 9 walls. The verges become grass (type and material 2) and those walls 5.
    // Materials drive only the dust, sounds, particles and stunt scoring, which know 0-2 and 8 (airborne: California
    // Speed puts 8 on road), so the ground's materials past 2 become road (0).
    constexpr size_t col_seg = 0x84, col_node = 0x14, col_poly = 0x18;

    void remap_surfaces(Bytes& col) {
        uint32_t segs = u16(col, 0), nodes = u16(col, 2), polys = u16(col, 4);
        size_t at = 12 + segs * col_seg + nodes * col_node;
        if (at + size_t(polys) * col_poly > col.size()) fail("collision polygons past the end of the file");
        for (uint32_t i = 0; i < polys; i++, at += col_poly) {
            uint8_t& low = col[at + 1];     // flags (u16, big-endian): type in bits 0-3, material in 4-7
            int type = low & 0xF, material = low >> 4;
            if (type == 3) {
                type = 2;
                material = 2;
            }
            else if (type == 4 || type >= 7) {
                type = 5;
            }
            if (type <= 2 && material > 2) material = 0;
            low = uint8_t(material << 4 | type);
        }
    }

    bool convert(const Bytes& rom, int t, const std::string& prefix, const std::set<std::string>& shared_models,
                 rush2::calspeed::Converted& out) {
        rush2::track2049::ConvertedTrack& track = out.track;
        out.record_models.clear();
        out.piece_models.clear();
        std::map<std::string, std::string> renamed;
        std::map<std::string, uint16_t> kinds;
        std::set<std::string> used;
        track.placement = convert_placement(asset(rom, placement_asset + t), prefix, out.record_models, renamed, kinds, used);

        // The track's container and the shared object containers its records draw from.
        std::vector<Bytes> files;
        files.push_back(asset(rom, geometry_asset + t));
        for (int a = first_object_asset; a <= last_object_asset; a++) {
            Bytes objects = asset(rom, a);
            std::set<std::string> names = rush2::track2049::model_names(objects);
            bool needed = std::any_of(used.begin(), used.end(), [&](const std::string& n) { return names.contains(n); }) ||
                          std::any_of(renamed.begin(), renamed.end(), [&](const auto& r) { return names.contains(r.first); });
            if (needed) files.push_back(std::move(objects));
        }
        std::vector<const Bytes*> sources;
        for (const Bytes& f : files) sources.push_back(&f);
        rush2::track2049::Rush2Merge spec;
        spec.rename = renamed;
        spec.dummies = { prefix + "FINISH", prefix + "FINISHB", "CSNONE" };
        spec.dummy_textures = { "CHKPNT", "FINISH" };
        spec.exclude = shared_models;
        spec.kinds = kinds;
        spec.default_kind = 0;
        std::set<std::string> names;
        std::string error;
        if (!rush2::track2049::merge_rush2_containers(sources, spec, track.geometry, names, error)) fail(error);
        for (const std::string& n : used) {
            if (!names.contains(n.substr(0, 15)) && !shared_models.contains(n.substr(0, 15))) {
                fail("placement record " + n + " has no model");
            }
        }
        // Rush 2's breakable pieces (resolved by name at race start) as California Speed's trees, cones and poles.
        auto piece = [&](const std::string& rush2, const std::string& cs) {
            if (names.contains(cs)) out.piece_models[rush2] = cs;
        };
        piece("TREEHIT1O1", "TREEHIT1I1");
        piece("TREEHIT2O1", "TREEHIT2I1");
        piece("TREEHIT3O1", "TREEHIT3I1");
        piece("TREEHIT4O1", "TREEHIT5I1");
        piece("CONE1O1", "CON17HIT1I1");
        piece("CURVEHITO1", names.contains("LAMPHIT1I1") ? "LAMPHIT1I1" : "LIGHTHIT1I1");

        track.collision = asset(rom, collision_asset + t);
        remap_surfaces(track.collision);
        track.path = asset(rom, path_asset + t);
        if (rush2::calspeed::point_to_point(t)) move_start(track.path);
        track.path_backward = track.path;

        int regions = 0;
        for (int k = 0; k < t; k++) regions += main_byte(rom, pvs_counts + k);
        int count = std::min<int>(main_byte(rom, pvs_counts + t), max_regions);
        track.pvs_count = uint8_t(count);
        track.pvs.clear();
        for (int i = 0; i < count * 16; i++) track.pvs.push_back(main_byte(rom, pvs_tables + uint32_t(regions * 16 + i)));
        for (int i = 0; i < 3; i++) track.fog[i] = main_byte(rom, fog_colours + uint32_t(t * 3 + i));
        out.cloud_scroll = rush2::calspeed::main_word(rom, cloud_scroll + uint32_t(t) * 4);
        out.sky_parallax = rush2::calspeed::main_word(rom, sky_parallax + uint32_t(t) * 4);
        // Attract-mode starts (unused by races): the spine's start.
        track.demo_starts[0] = { 0 };
        track.demo_starts[1] = { 0 };
        track.path_records.clear();
        track.spin_records.clear();
        track.prop_records.clear();
        track.pickup_records.clear();
        track.pool_records.clear();
        track.solid_triangles.clear();
        track.tex_anims = {};
        track.mover_paths.clear();
        track.mover_collision.clear();
        return true;
    }

    std::string extras_name(int t, const std::string& prefix) {
        return "calspeed_track" + std::to_string(t) + "_" + prefix;
    }

    std::mutex art_mutex;
    const Bytes* art_rom = nullptr;
    std::vector<rush2::track2049::MenuModel> art_models;
    std::vector<rush2::track2049::MenuLogo> art_logos;
    std::vector<float> art_radius;   // per track, its landmark's radius (0: none)
    std::vector<float> seeds;        // per track, lane 0's lap seconds (-1: not worked out)
}

std::string rush2::calspeed::menu_model_name(int t) {
    return "CSTRACK" + std::to_string(t + 1);
}

std::string rush2::calspeed::menu_logo_name(int t) {
    return "CSLOGO" + std::to_string(t + 1);
}

bool rush2::calspeed::load_track(const std::vector<uint8_t>& rom, int t, const std::string& prefix,
                                 const std::set<std::string>& shared_models, Converted& out, std::string& error) {
    if (t < 0 || t >= track_count) {
        error = "no such California Speed track";
        return false;
    }
    uint64_t source = rom_source(rom);
    Bytes extras;
    if (rush2::track_cache::load_2049(calspeed_k + t, prefix, source, out.track) &&
        rush2::track_cache::load_blob(extras_name(t, prefix), source, extras)) {
        Reader r{ extras };
        out.cloud_scroll = r.word();
        out.sky_parallax = r.word();
        out.record_models = r.map();
        out.piece_models = r.map();
        if (r.ok) return true;
    }
    try {
        convert(rom, t, prefix, shared_models, out);
    }
    catch (const ConvertError& e) {
        error = e.message;
        return false;
    }
    rush2::track_cache::save_2049(calspeed_k + t, prefix, source, out.track);
    Writer w;
    w.word(out.cloud_scroll);
    w.word(out.sky_parallax);
    w.map(out.record_models);
    w.map(out.piece_models);
    rush2::track_cache::save_blob(extras_name(t, prefix), source, w.b);
    return true;
}

bool rush2::calspeed::menu_art(const std::vector<uint8_t>& rom, std::vector<rush2::track2049::MenuModel>& models,
                               std::vector<rush2::track2049::MenuLogo>& logos) {
    std::lock_guard lock{ art_mutex };
    if (art_rom != &rom) {
        art_models.clear();
        art_logos.clear();
        art_radius.assign(track_count, 0.0f);
        std::vector<rush2::upscale::Image> images;
        uint64_t source = rom_source(rom);
        Bytes cached;
        bool loaded = false;
        if (rush2::track_cache::load_blob("calspeed_menu", source, cached)) {
            Reader r{ cached };
            uint32_t n = r.word();
            for (uint32_t i = 0; r.ok && i < n; i++) {
                rush2::track2049::MenuModel m;
                m.name = r.str();
                m.blob = r.bytes();
                m.list = r.word();
                m.pointers = r.words();
                uint32_t bits = r.word();
                memcpy(&m.radius, &bits, 4);
                m.loads = r.bytes();
                m.load_pointers = r.words();
                m.load_calls = r.words();
                art_models.push_back(std::move(m));
            }
            n = r.word();
            for (uint32_t i = 0; r.ok && i < n; i++) {
                rush2::track2049::MenuLogo logo;
                logo.name = r.str();
                logo.texels = r.bytes();
                logo.palette = r.bytes();
                rush2::upscale::Image image;
                image.width = logo_w;
                image.height = logo_h;
                image.rgba = r.bytes();
                art_logos.push_back(std::move(logo));
                images.push_back(std::move(image));
            }
            loaded = r.ok;
        }
        if (!loaded) {
            art_models.clear();
            art_logos.clear();
            images.clear();
            try {
                Bytes landmarks = asset(rom, landmark_asset), banners = asset(rom, banner_asset);
                for (int t = 0; t < track_count; t++) {
                    rush2::track2049::MenuModel m;
                    if (lift_model(landmarks, landmark_names[t], m)) {
                        m.name = menu_model_name(t);
                        art_models.push_back(std::move(m));
                    }
                    rush2::track2049::MenuLogo logo;
                    rush2::upscale::Image image;
                    if (build_logo(banners, t, logo, image)) {
                        art_logos.push_back(std::move(logo));
                        images.push_back(std::move(image));
                    }
                }
            }
            catch (const ConvertError& e) {
                printf("[CalSpeed] Track select art: %s\n", e.message.c_str());
            }
            Writer w;
            w.word(uint32_t(art_models.size()));
            for (const auto& m : art_models) {
                w.str(m.name);
                w.bytes(m.blob);
                w.word(m.list);
                w.words(m.pointers);
                uint32_t bits;
                memcpy(&bits, &m.radius, 4);
                w.word(bits);
                w.bytes(m.loads);
                w.words(m.load_pointers);
                w.words(m.load_calls);
            }
            w.word(uint32_t(art_logos.size()));
            for (size_t i = 0; i < art_logos.size(); i++) {
                w.str(art_logos[i].name);
                w.bytes(art_logos[i].texels);
                w.bytes(art_logos[i].palette);
                w.bytes(images[i].rgba);
            }
            rush2::track_cache::save_blob("calspeed_menu", source, w.b);
        }
        for (size_t i = 0; i < art_logos.size() && i < images.size(); i++) {
            rush2::upscale::add_exact_image(art_logos[i].texels, std::move(images[i]));
        }
        for (const auto& m : art_models) {
            int t = std::atoi(m.name.c_str() + 7) - 1;
            if (t >= 0 && t < track_count) art_radius[size_t(t)] = m.radius;
        }
        art_rom = &rom;
    }
    models = art_models;
    logos = art_logos;
    return !art_models.empty() || !art_logos.empty();
}

float rush2::calspeed::diorama_scale(const std::vector<uint8_t>& rom, int t) {
    std::lock_guard lock{ art_mutex };
    if (art_rom != &rom || t < 0 || t >= track_count || art_radius[size_t(t)] <= 0.5f) return 1.0f;
    return diorama_size / art_radius[size_t(t)];
}

bool rush2::calspeed::build_race_logo(const std::vector<uint8_t>& rush2_logo, const std::vector<uint8_t>& rom, int t,
                                      std::vector<uint8_t>& out) {
    std::vector<rush2::track2049::MenuModel> models;
    std::vector<rush2::track2049::MenuLogo> logos;
    menu_art(rom, models, logos);
    for (const auto& logo : logos) {
        if (logo.name == menu_logo_name(t)) return rush2::track2049::set_race_logo(rush2_logo, logo.texels, logo.palette, out);
    }
    return false;
}

float rush2::calspeed::record_seed(int t) {
    if (t < 0 || t >= track_count) return 0.0f;
    auto rom = get_rom();
    if (rom == nullptr) return 0.0f;
    std::lock_guard lock{ art_mutex };
    if (seeds.empty()) seeds.assign(track_count, -1.0f);
    if (seeds[size_t(t)] >= 0.0f) return seeds[size_t(t)];
    // Lane 0 of the path (docs/rush2049_research/race.md: route header 0x32C, branches, points, then 4 lanes of
    // {u16 count, u8, u8, u32} + count x {s16 x, y, z, u8 mph, u8}), driven at its speeds; a circuit's lap closes the
    // loop. The speeds are displayed mph, true mph x 1.2 (as Rush 2049's: track2049.cpp rush2_race_lane_speeds).
    float seconds = 0.0f;
    try {
        Bytes p = asset(*rom, path_asset + t);
        uint32_t spine = u16(p, 0x32C), branches = p.at(0x32C + 8);
        uint32_t o = 0x32C + 16, points = spine;
        for (uint32_t b = 0; b < branches; b++) points += u16(p, o + b * 16 + 0xA);
        o += 16 * branches + 2 + 6 * points;
        uint32_t lane = u16(p, o);
        o += 8;
        double time = 0.0;
        uint32_t steps = point_to_point(t) ? lane - 1 : lane;
        for (uint32_t i = 0; i < steps && lane > 1; i++) {
            uint32_t a = o + i * 8, b = o + ((i + 1) % lane) * 8;
            double dx = int16_t(u16(p, b)) - int16_t(u16(p, a)), dy = int16_t(u16(p, b + 2)) - int16_t(u16(p, a + 2)),
                   dz = int16_t(u16(p, b + 4)) - int16_t(u16(p, a + 4));
            int mph = p.at(a + 6);
            if (mph > 0) time += std::sqrt(dx * dx + dy * dy + dz * dz) / (mph / 1.2 * 5280.0 / 3600.0);
        }
        seconds = float(time);
    }
    catch (const std::exception&) {
    }
    catch (const ConvertError&) {
    }
    seeds[size_t(t)] = seconds;
    return seconds;
}
