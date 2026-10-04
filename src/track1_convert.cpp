// Conversion of a San Francisco Rush (Rush 1) race track to the files of a Rush 2 track slot. Port of
// tools/rush1/track1.py; its output is byte-identical (tools/rush1/cpp_test checks this). Formats and evidence are in
// docs/rush1_research.md.
//
// - Geometry: Rush 1's model containers are Rush 2's records (models, names, textures, palettes) behind a smaller
//   header, with segment addresses instead of file offsets, and F3DEX 1.x display lists instead of F3DEX2. Rush 2's
//   loader rebases a model's pointers by walking its list without entering G_DL calls, plus the SETTIMGs of one
//   linear range of texture-load lists, so every model list is flattened into a new list: called lists are inlined,
//   except lists that only load textures, which become calls into that range. Commands are translated to F3DEX2 in
//   place (all are 8 bytes in both). The track's segment also covers Rush 1's shared track texture bank (asset 36),
//   which Rush 1 loads right after the track geometry.
// - Placement: Rush 2's record layout. The section chain is kept as it is (its order numbers the visibility regions);
//   Rush 1 objects that Rush 2 also has become Rush 2's classes, its sound emitters Rush 2's emitters.
// - Collision: Rush 2's sections with a longer header and polygons, in a different space: Rush 1 collision space is
//   (render z, render x, -render y) and its polygon frames keep the surface in local x/y with depth along z, where
//   Rush 2 uses local z/x and height along y. Rush 2 local = (y, -z, x) of Rush 1 local, for polygons and road
//   segments alike.
// - AI path: Rush 1 has four lanes of AI points and a checkpoint list in code; Rush 2's path file is built from them.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "track1.h"

namespace {
    using Bytes = std::vector<uint8_t>;

    constexpr int geometry_asset = 37, placement_asset = 0, collision_asset = 44, collision_back_asset = 51;
    constexpr int path_asset = 58, path_back_asset = 65, objects_asset = 12, texture_bank_asset = 36;
    constexpr int diorama_asset = 10;
    constexpr uint32_t finish_models = 0x800C7AA0;            // char*[7]
    constexpr uint32_t checkpoint_lists[2] = { 0x800C7B6C, 0x800C7B88 };
    constexpr uint32_t pvs_tables[7] = { 0x800CD0F8, 0x800CD488, 0x800CDB78, 0x800CE108, 0x800CE7F8, 0x800CEE38, 0x800CF298 };
    constexpr uint32_t pvs_counts = 0x800CF77C;

    // Rush 2's placement name classifier (func_800815DC): exact names, then prefixes.
    const char* const r2_exact[] = { "BIGCHR1", "BIGCHR2", "BOAT", "CABLECAR", "CANNON", "CROWDSCR", "DOCKWHIS",
        "FIRECRCK", "FOGHORN", "FOUNTAIN", "KIDSPLAY", "OCEAN", "PARKBIRD", "SEAGULL", "SMLCLAP", "SMLHOOT", "VOLCANO",
        "KLAX", "HARDRIVE", "PETERP", "PITFIGTER", "RAMPART", "MARBLE" };
    const char* const r2_prefixes[] = { "MARKER", "TIME", "COLLISION", "CONE1", "FENCE", "FLAG2", "GASIGN", "GATE",
        "KEY", "METER", "TREEHIT", "WINDOW", "BALL", "SHATPANE", "CURVEHIT", "THINKHIT", "BUMPHIT", "DIPHIT", "PCAREHIT",
        "NOPASHIT", "RIGHTHIT", "LEFTHIT", "ZONEHIT", "MPH45HIT", "MPH75HIT", "YIELDHIT", "REDUCHIT", "STOPHIT",
        "SLOWHIT", "NYTREEHT", "GRANDWIN", "GLAMPHIT", "CHAIRHIT", "DESKHIT", "MAPSIGN", "SRFBRD", "USFLAG", "UMBRELLA",
        "RATCONE", "P737", "PJET", "F23", "ENGTABLE", "DOTHEDEW", "NYLGATE" };

    // Rush 1 objects that Rush 2 also has: name prefix -> Rush 2 class (Rush 2's model and behaviour), or "" to leave
    // the record out; Rush 1 sound emitters -> Rush 2 emitters.
    const std::pair<const char*, const char*> r1_objects[] = { { "CONE1L", "CONE1" }, { "METERL", "METER" },
        { "TREEHIT", "TREEHIT" }, { "FLAG2L", "FLAG2" }, { "WINDOWBL", "SHATPANE" }, { "KEYL", "" }, { "MARKER", "" },
        { "TIME", "" } };
    const std::pair<const char*, const char*> r1_emitters[] = { { "CCAR", "CABLECAR" }, { "FIRECRK", "FIRECRCK" },
        { "SMALLHOOT", "SMLHOOT" }, { "BIGCHEER", "BIGCHR1" }, { "BIGCHEER2", "BIGCHR2" } };

    struct ConvertError {
        std::string message;
    };

    [[noreturn]] void fail(const std::string& message) {
        throw ConvertError{ message };
    }

    std::string hex(uint64_t v) {
        char s[24];
        snprintf(s, sizeof(s), "%llX", (unsigned long long)v);
        return s;
    }

    uint32_t u32(const Bytes& d, size_t o) {
        if (o + 4 > d.size()) fail("read past the end at " + hex(o));
        return (uint32_t(d[o]) << 24) | (uint32_t(d[o + 1]) << 16) | (uint32_t(d[o + 2]) << 8) | d[o + 3];
    }
    uint16_t u16(const Bytes& d, size_t o) {
        if (o + 2 > d.size()) fail("read past the end at " + hex(o));
        return uint16_t((d[o] << 8) | d[o + 1]);
    }
    int16_t s16(const Bytes& d, size_t o) { return int16_t(u16(d, o)); }
    float f32(const Bytes& d, size_t o) {
        uint32_t b = u32(d, o);
        float f;
        memcpy(&f, &b, 4);
        return f;
    }
    void put32(Bytes& d, size_t o, uint32_t v) {
        d[o] = uint8_t(v >> 24); d[o + 1] = uint8_t(v >> 16); d[o + 2] = uint8_t(v >> 8); d[o + 3] = uint8_t(v);
    }
    void put16(Bytes& d, size_t o, uint16_t v) {
        d[o] = uint8_t(v >> 8); d[o + 1] = uint8_t(v);
    }
    void putf(Bytes& d, size_t o, double v) {
        float f = float(v);
        uint32_t b;
        memcpy(&b, &f, 4);
        put32(d, o, b);
    }
    void add32(Bytes& d, uint32_t v) {
        d.push_back(uint8_t(v >> 24)); d.push_back(uint8_t(v >> 16)); d.push_back(uint8_t(v >> 8)); d.push_back(uint8_t(v));
    }
    void add16(Bytes& d, uint16_t v) {
        d.push_back(uint8_t(v >> 8)); d.push_back(uint8_t(v));
    }
    void addf(Bytes& d, double v) {
        float f = float(v);
        uint32_t b;
        memcpy(&b, &f, 4);
        add32(d, b);
    }
    void align8(Bytes& d) {
        while (d.size() % 8) d.push_back(0);
    }
    std::string cname(const Bytes& d, size_t o, size_t n = 16) {
        if (o + n > d.size()) fail("name past the end");
        return std::string(reinterpret_cast<const char*>(&d[o]), strnlen(reinterpret_cast<const char*>(&d[o]), n));
    }
    void put_name(Bytes& d, size_t o, const std::string& name) {
        memset(&d[o], 0, 16);
        memcpy(&d[o], name.data(), std::min<size_t>(name.size(), 16));
    }
    bool starts_with(const std::string& s, const char* p) {
        return s.rfind(p, 0) == 0;
    }
    // Rush 2 sorts names by their first 15 bytes (the lookup compares 15 characters).
    bool name_less(const std::string& a, const std::string& b) {
        return a.substr(0, 15) < b.substr(0, 15);
    }

    // Values in Rush 1's main code.
    struct Main {
        const Bytes& d;
        uint32_t w(uint32_t vram) const { return u32(d, vram - rush2::track1::main_vram); }
        uint8_t b(uint32_t vram) const {
            size_t o = vram - rush2::track1::main_vram;
            if (o >= d.size()) fail("main code read past the end");
            return d[o];
        }
        std::string s(uint32_t vram) const { return cname(d, vram - rush2::track1::main_vram, 32); }
    };

    bool r2_classified(const std::string& name) {
        for (const char* e : r2_exact) if (name == e) return true;
        for (const char* p : r2_prefixes) if (starts_with(name, p)) return true;
        return false;
    }

    std::string safe_name(const std::string& name) {
        if (!r2_classified(name)) return name;
        std::string out = "R1" + name;
        if (out.size() > 15 || r2_classified(out)) fail("no safe name for " + name);
        return out;
    }

    // ---------------------------------------------------------------------------------------------------------------
    // Model containers

    struct Lod {
        uint16_t flags;
        float dist;
        uint32_t list;   // Offset, or 0.
    };

    // A Rush 1 model container: 0x20-byte header {names, model count, 0, 0, textures, count, palettes, count},
    // 0x34-byte model records from +0x20, 0x18 name, 0x20 texture and 0x18 palette records; pointers are addresses
    // in the file's own segment.
    struct Container {
        Bytes d;
        uint32_t seg = 0, n_models = 0, names_at = 0, tex_at = 0, n_tex = 0, pal_at = 0, n_pal = 0;

        explicit Container(Bytes data) : d(std::move(data)) {
            if (d.size() < 0x20) fail("container too small");
            seg = u32(d, 0) >> 24;
            n_models = u32(d, 4);
            names_at = off(u32(d, 0));
            tex_at = off(u32(d, 16));
            n_tex = u32(d, 20);
            pal_at = off(u32(d, 24));
            n_pal = u32(d, 28);
            if (0x20 + size_t(n_models) * 0x34 > d.size() || names_at + size_t(n_models) * 0x18 > d.size() ||
                tex_at + size_t(n_tex) * 0x20 > d.size() || pal_at + size_t(n_pal) * 0x18 > d.size()) {
                fail("container tables out of range");
            }
        }
        uint32_t off(uint32_t a) const {
            if ((a >> 24) != seg || (a & 0xFFFFFF) >= d.size()) fail("pointer " + hex(a) + " outside the file");
            return a & 0xFFFFFF;
        }
        uint32_t lod_count(uint32_t i) const { return u32(d, 0x20 + i * 0x34); }
        Lod lod(uint32_t i, int l) const {
            size_t o = 0x20 + i * 0x34 + 4 + l * 12;
            uint32_t dl = u32(d, o + 8);
            return { u16(d, o + 2), f32(d, o + 4), dl ? off(dl) : 0 };
        }
        std::string name(uint32_t i) const { return cname(d, names_at + i * 0x18); }
        float radius(uint32_t i) const { return f32(d, names_at + i * 0x18 + 16); }
        std::string texture_name(uint32_t i) const { return cname(d, tex_at + i * 0x20); }
        Bytes texture(uint32_t i) const { return Bytes(d.begin() + tex_at + i * 0x20, d.begin() + tex_at + (i + 1) * 0x20); }
        std::string palette_name(uint32_t i) const { return cname(d, pal_at + i * 0x18); }
        Bytes palette(uint32_t i) const { return Bytes(d.begin() + pal_at + i * 0x18, d.begin() + pal_at + (i + 1) * 0x18); }
    };

    // F3DEX (1.x) commands, and their F3DEX2 translation.
    constexpr uint8_t g1_vtx = 0x04, g1_dl = 0x06, g1_branch_z = 0xB0, g1_tri2 = 0xB1, g1_rdphalf_1 = 0xB4,
        g1_enddl = 0xB8, g1_othermode_l = 0xB9, g1_othermode_h = 0xBA, g1_texture = 0xBB, g1_tri1 = 0xBF,
        g1_spnoop = 0x00, g1_cleargeometrymode = 0xB6, g1_setgeometrymode = 0xB7;

    uint32_t geometry_bits(uint32_t m) {
        // Bits that moved between F3DEX and F3DEX2: shading smooth, cull front, cull back.
        const std::pair<uint32_t, uint32_t> moved[] = { { 0x200, 0x200000 }, { 0x1000, 0x200 }, { 0x2000, 0x400 } };
        uint32_t out = m;
        for (auto [a, b] : moved) out &= ~a;
        for (auto [a, b] : moved) if (m & a) out |= b;
        return out;
    }

    std::pair<uint32_t, uint32_t> translate(uint32_t w0, uint32_t w1) {
        uint8_t op = uint8_t(w0 >> 24);
        switch (op) {
            case g1_vtx: {
                uint32_t n = (w0 >> 10) & 0x3F, v0 = ((w0 >> 16) & 0xFF) / 2;
                if ((w0 & 0x3FF) != n * 16 - 1 || n == 0 || v0 + n > 32) fail("odd G_VTX " + hex(w0));
                return { 0x01000000 | (n << 12) | ((v0 + n) << 1), w1 };
            }
            case g1_tri1: return { 0x05000000 | (w1 & 0xFFFFFF), 0 };
            case g1_tri2: return { 0x06000000 | (w0 & 0xFFFFFF), w1 };
            case g1_othermode_l:
            case g1_othermode_h: {
                uint32_t sft = (w0 >> 8) & 0xFF, len = w0 & 0xFF;
                return { (uint32_t(op == g1_othermode_l ? 0xE2 : 0xE3) << 24) | ((32 - sft - len) << 8) | (len - 1), w1 };
            }
            case g1_texture: return { 0xD7000000 | (w0 & 0xFFFF00) | ((w0 & 0xFF) << 1), w1 };
            case g1_setgeometrymode: return { 0xD9FFFFFF, geometry_bits(w1) };
            case g1_cleargeometrymode: return { 0xD9000000 | (~geometry_bits(w1) & 0xFFFFFF), 0 };
            case g1_spnoop: return { 0xE0000000, 0 };
            case g1_enddl: return { 0xDF000000, 0 };
        }
        if (op >= 0xE6) return { w0, w1 };   // RDP commands, the same in both (not the texture rectangles)
        fail("unsupported F3DEX command " + hex(w0) + " " + hex(w1));
    }

    // Rush 1 lists switch the texture LUT and texture LOD and don't always switch them back; Rush 2 expects the LUT on
    // RGBA16 (set once per frame, which its car lists rely on) and LOD off after a model.
    const uint32_t restore_state[4] = { 0xE3001001, 0x8000, 0xE3000F00, 0 };

    struct Ref {
        bool load;       // Index into the texture-load lists, else offset in a source.
        int index;       // Load list, or source.
        uint32_t offset;
    };

    struct Fixup {
        size_t pos;
        Ref ref;
    };

    struct Body {
        Bytes bytes;
        std::vector<Fixup> fixups;
    };

    // Builds a Rush 2 container from Rush 1 containers (see the file comment). Layout: 0x28-byte header, each source
    // file verbatim (vertices, texels and palettes stay put), the texture-load lists ([7]..[8]), the model lists in
    // name order, then the model, texture, palette and name tables.
    class Builder {
    public:
        struct ModelOut {
            std::string name;
            uint32_t count;
            std::optional<Body> lods[4];
            uint16_t flags[4];
            float dist[4];
            float radius;
        };
        struct TextureOut {
            std::string name;
            Bytes rec;
            Ref ref;
            std::string palette;
            int source;
        };
        struct PaletteOut {
            std::string name;
            Bytes rec;
            int source;
        };

        Bytes out = Bytes(0x28, 0);
        // Appending to an existing Rush 2 container instead (track select art): its records stay as they are, and
        // only lists without calls are added, so its texture-load range doesn't change.
        bool extending = false;
        struct RawModel { std::string name; Bytes rec, name_rec; };
        struct RawTexture { std::string name; Bytes rec; std::string palette; };
        struct RawPalette { std::string name; Bytes rec; };
        std::vector<RawModel> raw_models;
        std::vector<RawTexture> raw_textures;
        std::vector<RawPalette> raw_palettes;
        std::vector<std::pair<const Container*, uint32_t>> sources;
        std::map<std::pair<int, uint32_t>, int> loads;
        std::vector<Body> load_lists;
        std::vector<ModelOut> models;
        std::vector<TextureOut> textures;
        std::vector<PaletteOut> palettes;

        int add_source(const Container& c) {
            align8(out);
            sources.push_back({ &c, uint32_t(out.size()) });
            out.insert(out.end(), c.d.begin(), c.d.end());
            return int(sources.size() - 1);
        }

        std::pair<uint32_t, uint32_t> cmd(int s, uint32_t o) const {
            const Bytes& d = sources[s].first->d;
            return { u32(d, o), u32(d, o + 4) };
        }

        // A list that only sets state and loads textures (no vertices, triangles or branches).
        bool is_load_list(int s, uint32_t o, int depth = 0) const {
            const Container& c = *sources[s].first;
            for (int i = 0; i < 4096; i++) {
                auto [w0, w1] = cmd(s, o);
                uint8_t op = uint8_t(w0 >> 24);
                if (op == g1_enddl) return true;
                if (op == g1_dl) {
                    if (depth > 8 || !is_load_list(s, c.off(w1), depth + 1)) return false;
                    if (((w0 >> 16) & 0xFF) == 1) return true;
                }
                else if (op == g1_vtx || op == g1_tri1 || op == g1_tri2 || op == g1_rdphalf_1 || op == g1_branch_z ||
                         op == 0xE4 || op == 0xE5) {
                    return false;
                }
                o += 8;
            }
            return false;
        }

        int load_list(int s, uint32_t target, int depth) {
            auto key = std::make_pair(s, target);
            auto it = loads.find(key);
            if (it != loads.end()) return it->second;
            Body body;
            emit(s, target, body, false, depth + 1);
            add32(body.bytes, 0xDF000000);
            add32(body.bytes, 0);
            int index = int(load_lists.size());
            loads[key] = index;
            load_lists.push_back(std::move(body));
            return index;
        }

        // Appends the flattened, translated list at source offset o (without its G_ENDDL).
        void emit(int s, uint32_t o, Body& out_body, bool loads_ok, int depth = 0) {
            const Container& c = *sources[s].first;
            if (depth > 16) fail("display lists nest too deep");
            for (int i = 0; i < 65536; i++) {
                auto [w0, w1] = cmd(s, o);
                uint8_t op = uint8_t(w0 >> 24);
                if (op == g1_enddl) return;
                if (op == g1_dl) {
                    uint32_t target = c.off(w1);
                    if (loads_ok && is_load_list(s, target)) {
                        int index = load_list(s, target, depth);
                        out_body.fixups.push_back({ out_body.bytes.size() + 4, { true, index, 0 } });
                        add32(out_body.bytes, 0xDE000000);
                        add32(out_body.bytes, 0);
                    }
                    else {
                        emit(s, target, out_body, loads_ok, depth + 1);
                    }
                    if (((w0 >> 16) & 0xFF) == 1) return;
                    o += 8;
                    continue;
                }
                if (op == g1_rdphalf_1) {
                    if ((cmd(s, o + 8).first >> 24) != g1_branch_z) fail("RDPHALF_1 without BRANCH_Z at " + hex(o));
                    emit(s, c.off(w1), out_body, loads_ok, depth + 1);
                    return;
                }
                auto [nw0, nw1] = translate(w0, w1);
                if (op == g1_vtx || op == 0xFD) {
                    out_body.fixups.push_back({ out_body.bytes.size() + 4, { false, s, c.off(w1) } });
                    nw1 = 0;
                }
                add32(out_body.bytes, nw0);
                add32(out_body.bytes, nw1);
                o += 8;
            }
            fail("display list at " + hex(o) + " has no end");
        }

        void add_model(int s, uint32_t i, const std::string& name, bool loads_ok = true) {
            const Container& c = *sources[s].first;
            ModelOut m{ name, c.lod_count(i), {}, {}, {}, c.radius(i) };
            for (int l = 0; l < 4; l++) {
                Lod lod = c.lod(i, l);
                m.flags[l] = lod.flags & 0x7;
                m.dist[l] = lod.dist;
                if (lod.list == 0) continue;
                Body body;
                emit(s, lod.list, body, loads_ok);
                for (uint32_t w : restore_state) add32(body.bytes, w);
                add32(body.bytes, 0xDF000000);
                add32(body.bytes, 0);
                m.lods[l] = std::move(body);
            }
            models.push_back(std::move(m));
        }

        void add_empty_model(const std::string& name) {
            ModelOut m{ name, 1, {}, {}, {}, 0.0f };
            Body body;
            add32(body.bytes, 0xDF000000);
            add32(body.bytes, 0);
            m.lods[0] = std::move(body);
            models.push_back(std::move(m));
        }

        void add_texture(int s, uint32_t i, const std::string& name) {
            const Container& c = *sources[s].first;
            Bytes rec = c.texture(i);
            int16_t pal = s16(rec, 22);
            std::string palname = pal >= 0 && uint32_t(pal) < c.n_pal ? c.palette_name(uint32_t(pal)) : std::string();
            uint32_t target = c.off(u32(rec, 24));
            Ref ref = is_load_list(s, target) ? Ref{ true, load_list(s, target, 0), 0 } : Ref{ false, s, target };
            textures.push_back({ name, rec, ref, palname, s });
        }

        void add_palette(int s, const std::string& name, const std::string& new_name) {
            const Container& c = *sources[s].first;
            for (uint32_t i = 0; i < c.n_pal; i++) {
                if (c.palette_name(i) == name) {
                    palettes.push_back({ new_name, c.palette(i), s });
                    return;
                }
            }
            fail("no palette " + name);
        }

        // Starts from an existing Rush 2 container (header, data, tables).
        void extend(const Bytes& base) {
            if (base.size() < 0x28) fail("container too small");
            uint32_t h[10];
            for (int i = 0; i < 10; i++) h[i] = u32(base, i * 4);
            if (h[0] + size_t(h[4]) * 0x34 > base.size() || h[1] + size_t(h[4]) * 0x18 > base.size() ||
                h[2] + size_t(h[5]) * 0x20 > base.size() || h[3] + size_t(h[6]) * 0x18 > base.size()) {
                fail("container tables out of range");
            }
            out = base;
            extending = true;
            for (uint32_t i = 0; i < h[4]; i++) {
                raw_models.push_back({ cname(base, h[1] + i * 0x18),
                                       Bytes(base.begin() + h[0] + i * 0x34, base.begin() + h[0] + (i + 1) * 0x34),
                                       Bytes(base.begin() + h[1] + i * 0x18, base.begin() + h[1] + (i + 1) * 0x18) });
            }
            for (uint32_t i = 0; i < h[6]; i++) {
                raw_palettes.push_back({ cname(base, h[3] + i * 0x18),
                                         Bytes(base.begin() + h[3] + i * 0x18, base.begin() + h[3] + (i + 1) * 0x18) });
            }
            for (uint32_t i = 0; i < h[5]; i++) {
                Bytes rec(base.begin() + h[2] + i * 0x20, base.begin() + h[2] + (i + 1) * 0x20);
                int16_t pal = s16(rec, 22);
                raw_textures.push_back({ cname(rec, 0), rec,
                                         pal >= 0 && uint32_t(pal) < h[6] ? raw_palettes[size_t(pal)].name : std::string() });
            }
        }

        // A texture whose data is texels, with a palette of the same name, placed in the container.
        void add_texels(const std::string& name, uint16_t w, uint16_t h, uint8_t fmt, uint8_t siz, uint32_t flags,
                        const Bytes& texels, const Bytes& palette) {
            align8(out);
            uint32_t texel_off = uint32_t(out.size());
            out.insert(out.end(), texels.begin(), texels.end());
            uint32_t palette_off = uint32_t(out.size());
            out.insert(out.end(), palette.begin(), palette.end());
            Bytes trec(0x20, 0), prec(0x18, 0);
            put_name(trec, 0, name);
            put16(trec, 16, w);
            put16(trec, 18, h);
            trec[20] = fmt;
            trec[21] = siz;
            put32(trec, 24, texel_off);
            put32(trec, 28, flags);
            put_name(prec, 0, name);
            put32(prec, 16, 0x00FF8000);
            put32(prec, 20, palette_off);
            raw_textures.push_back({ name, trec, name });
            raw_palettes.push_back({ name, prec });
        }

        // The extended container: the new model lists, then every table rebuilt (old and new records, sorted).
        Bytes build_extended() {
            if (!load_lists.empty() || !textures.empty() || !palettes.empty()) fail("extension with texture-load lists");
            align8(out);
            std::vector<uint32_t> no_loads;
            for (ModelOut& m : models) {
                Bytes rec, name_rec(0x18, 0);
                add32(rec, m.count);
                for (int l = 0; l < 4; l++) {
                    uint32_t at = 0;
                    if (m.lods[l]) {
                        at = uint32_t(out.size());
                        out.insert(out.end(), m.lods[l]->bytes.begin(), m.lods[l]->bytes.end());
                        for (const Fixup& f : m.lods[l]->fixups) put32(out, at + f.pos, resolve(f.ref, no_loads));
                    }
                    add16(rec, 0);
                    add16(rec, m.flags[l]);
                    addf(rec, m.dist[l]);
                    add32(rec, at);
                }
                put_name(name_rec, 0, m.name);
                putf(name_rec, 16, m.radius);
                raw_models.push_back({ m.name, rec, name_rec });
            }
            auto by_name = [](const auto& a, const auto& b) { return name_less(a.name, b.name); };
            std::stable_sort(raw_models.begin(), raw_models.end(), by_name);
            std::stable_sort(raw_textures.begin(), raw_textures.end(), by_name);
            std::stable_sort(raw_palettes.begin(), raw_palettes.end(), by_name);
            for (size_t i = 1; i < raw_models.size(); i++) {
                if (raw_models[i - 1].name.substr(0, 15) == raw_models[i].name.substr(0, 15)) {
                    fail("duplicate model name " + raw_models[i].name);
                }
            }
            uint32_t model_at = uint32_t(out.size());
            for (const RawModel& m : raw_models) out.insert(out.end(), m.rec.begin(), m.rec.end());
            uint32_t tex_at = uint32_t(out.size());
            for (RawTexture& t : raw_textures) {
                int16_t index = -1;
                for (size_t i = 0; i < raw_palettes.size(); i++) {
                    if (raw_palettes[i].name == t.palette) { index = int16_t(i); break; }
                }
                put16(t.rec, 22, uint16_t(index));
                out.insert(out.end(), t.rec.begin(), t.rec.end());
            }
            uint32_t pal_at = uint32_t(out.size());
            for (const RawPalette& p : raw_palettes) out.insert(out.end(), p.rec.begin(), p.rec.end());
            uint32_t name_at = uint32_t(out.size());
            for (const RawModel& m : raw_models) out.insert(out.end(), m.name_rec.begin(), m.name_rec.end());
            put32(out, 0, model_at);
            put32(out, 4, name_at);
            put32(out, 8, tex_at);
            put32(out, 12, pal_at);
            put32(out, 16, uint32_t(raw_models.size()));
            put32(out, 20, uint32_t(raw_textures.size()));
            put32(out, 24, uint32_t(raw_palettes.size()));
            return std::move(out);
        }

        uint32_t resolve(const Ref& ref, const std::vector<uint32_t>& load_at) const {
            return ref.load ? load_at[size_t(ref.index)] : sources[size_t(ref.index)].second + ref.offset;
        }

        Bytes build() {
            if (extending) return build_extended();
            align8(out);
            std::vector<uint32_t> load_at;
            uint32_t start7 = uint32_t(out.size());
            for (const Body& b : load_lists) {
                load_at.push_back(uint32_t(out.size()));
                out.insert(out.end(), b.bytes.begin(), b.bytes.end());
            }
            uint32_t end8 = uint32_t(out.size());
            for (size_t i = 0; i < load_lists.size(); i++) {
                for (const Fixup& f : load_lists[i].fixups) put32(out, load_at[i] + f.pos, resolve(f.ref, load_at));
            }
            std::stable_sort(models.begin(), models.end(), [](const ModelOut& a, const ModelOut& b) { return name_less(a.name, b.name); });
            for (size_t i = 1; i < models.size(); i++) {
                if (models[i - 1].name.substr(0, 15) == models[i].name.substr(0, 15)) fail("duplicate model name " + models[i].name);
            }
            std::vector<Bytes> recs;
            for (const ModelOut& m : models) {
                Bytes rec;
                add32(rec, m.count);
                for (int l = 0; l < 4; l++) {
                    uint32_t at = 0;
                    if (m.lods[l]) {
                        at = uint32_t(out.size());
                        out.insert(out.end(), m.lods[l]->bytes.begin(), m.lods[l]->bytes.end());
                        for (const Fixup& f : m.lods[l]->fixups) put32(out, at + f.pos, resolve(f.ref, load_at));
                    }
                    add16(rec, 0);
                    add16(rec, m.flags[l]);
                    addf(rec, m.dist[l]);
                    add32(rec, at);
                }
                recs.push_back(std::move(rec));
            }
            std::stable_sort(textures.begin(), textures.end(), [](const TextureOut& a, const TextureOut& b) { return a.name < b.name; });
            std::stable_sort(palettes.begin(), palettes.end(), [](const PaletteOut& a, const PaletteOut& b) { return a.name < b.name; });
            uint32_t model_at = uint32_t(out.size());
            for (const Bytes& r : recs) out.insert(out.end(), r.begin(), r.end());
            uint32_t tex_at = uint32_t(out.size());
            for (const TextureOut& t : textures) {
                Bytes r = t.rec;
                put_name(r, 0, t.name);
                if (u16(r, 20) == 0) put16(r, 20, 0x0002);
                int16_t index = -1;
                for (size_t i = 0; i < palettes.size(); i++) {
                    if (palettes[i].name == t.palette) { index = int16_t(i); break; }
                }
                put16(r, 22, uint16_t(index));
                put32(r, 24, resolve(t.ref, load_at));
                put32(r, 28, 0);
                out.insert(out.end(), r.begin(), r.end());
            }
            uint32_t pal_at = uint32_t(out.size());
            for (const PaletteOut& p : palettes) {
                Bytes r = p.rec;
                put_name(r, 0, p.name);
                put32(r, 20, sources[size_t(p.source)].second + sources[size_t(p.source)].first->off(u32(p.rec, 20)));
                out.insert(out.end(), r.begin(), r.end());
            }
            uint32_t name_at = uint32_t(out.size());
            for (const ModelOut& m : models) {
                size_t o = out.size();
                out.resize(o + 0x18, 0);
                put_name(out, o, m.name);
                putf(out, o + 16, m.radius);
            }
            uint32_t h[10] = { model_at, name_at, tex_at, pal_at, uint32_t(models.size()), uint32_t(textures.size()),
                               uint32_t(palettes.size()), start7, end8, 0 };
            for (int i = 0; i < 10; i++) put32(out, i * 4, h[i]);
            return std::move(out);
        }
    };

    bool read(const std::vector<uint8_t>& rom, int index, Bytes& out) {
        return rush2::track1::read_asset(rom, index, out);
    }

    Bytes asset(const std::vector<uint8_t>& rom, int index) {
        Bytes out;
        if (!read(rom, index, out)) fail("can't read Rush 1 asset " + std::to_string(index));
        return out;
    }

    Bytes convert_geometry(const std::vector<uint8_t>& rom, const Main& main, int t, const std::string& prefix,
                           std::map<std::string, std::string>& names) {
        Bytes track_data = asset(rom, geometry_asset + t), bank = asset(rom, texture_bank_asset);
        track_data.insert(track_data.end(), bank.begin(), bank.end());
        Container track(std::move(track_data));
        Container objects(asset(rom, objects_asset));
        std::string finish = main.s(main.w(finish_models + t * 4));
        Builder b;
        int st = b.add_source(track);
        int so = b.add_source(objects);
        for (uint32_t i = 0; i < track.n_models; i++) {
            std::string n = track.name(i);
            std::string renamed = n == finish ? prefix + "FINISH" : safe_name(n);
            names[n] = renamed;
            b.add_model(st, i, renamed);
        }
        if (!names.count(finish)) fail("finish model " + finish + " missing");
        b.add_empty_model("R1EMPTY");
        for (uint32_t i = 0; i < track.n_tex; i++) {
            if (track.texture_name(i) == "CHKPOINT") {
                b.add_texture(st, i, "CHKPNT");
                b.add_palette(st, "CHKPOINT", "CHKPOINT");
            }
        }
        for (uint32_t i = 0; i < objects.n_tex; i++) {
            if (objects.texture_name(i) == "FINISH") {
                b.add_texture(so, i, "FINISH");
                b.add_palette(so, "FINISH", "FINISH");
            }
        }
        if (b.textures.size() != 2) fail("CHKPOINT/FINISH textures missing");
        return b.build();
    }

    // ---------------------------------------------------------------------------------------------------------------
    // Placement

    Bytes convert_placement(const std::vector<uint8_t>& rom, int t, const std::string& prefix,
                            const std::map<std::string, std::string>& names) {
        Bytes d = asset(rom, placement_asset + t);
        if (u32(d, 0) != 1) fail("placement has " + std::to_string(u32(d, 0)) + " trees");
        uint32_t base = u32(d, 4);
        size_t n_recs = (d.size() - base) / 0x64;
        auto rec = [&](int i) {
            if (i < 0 || size_t(i) >= n_recs) fail("placement record " + std::to_string(i) + " out of range");
            return Bytes(d.begin() + base + size_t(i) * 0x64, d.begin() + base + size_t(i + 1) * 0x64);
        };
        auto name = [&](int i) { return cname(d, base + size_t(i) * 0x64); };
        auto next = [&](int i) { return int(s16(rec(i), 0x44)); };
        auto child = [&](int i) { return int(s16(rec(i), 0x46)); };
        std::vector<int> top;
        for (int i = 0; i >= 0; i = next(i)) {
            top.push_back(i);
            if (top.size() > n_recs) fail("placement chain loops");
        }
        struct Section { Bytes rec; std::vector<Bytes> kids; };
        std::vector<Section> sections;
        for (int i : top) {
            auto it = names.find(name(i));
            if (it == names.end()) fail("section " + name(i) + " has no model");
            Bytes r0 = rec(i);
            put_name(r0, 0, it->second);
            put32(r0, 0x40, u32(r0, 0x40) & ~0x1000u);
            put32(r0, 0x48, 0);
            bool identity = true;
            const float id[9] = { 1, 0, 0, 0, 1, 0, 0, 0, 1 };
            for (int k = 0; k < 9; k++) identity = identity && f32(r0, 0x10 + k * 4) == id[k];
            if (!identity && child(i) >= 0) fail("rotated section " + name(i) + " has children");
            float ppos[3] = { f32(r0, 0x34), f32(r0, 0x38), f32(r0, 0x3C) };
            Section sec{ r0, {} };
            for (int c = child(i); c >= 0; c = next(c)) {
                if (child(c) >= 0) fail("nested children under " + name(i));
                Bytes k = rec(c);
                std::string n = name(c);
                std::optional<std::string> renamed;
                bool world = false;
                for (auto [from, to] : r1_emitters) {
                    if (n == from) { renamed = to; world = true; }
                }
                if (!world) {
                    bool matched = false;
                    for (auto [p, mapped] : r1_objects) {
                        if (starts_with(n, p)) {
                            matched = true;
                            world = true;
                            if (*mapped) renamed = mapped;
                            break;
                        }
                    }
                    if (!matched) {
                        auto nt = names.find(n);
                        if (nt == names.end()) fail("object " + n + " has no model");
                        renamed = nt->second;
                    }
                }
                if (!renamed) continue;
                put_name(k, 0, *renamed);
                if (world) {
                    for (int a = 0; a < 3; a++) putf(k, 0x34 + a * 4, double(f32(k, 0x34 + a * 4)) + double(ppos[a]));
                }
                put32(k, 0x40, world ? 0x40 : (u32(k, 0x40) & ~0x1000u));
                put32(k, 0x48, 0);
                for (int a = 0; a < 6; a++) putf(k, 0x4C + a * 4, 0.0);
                sec.kids.push_back(std::move(k));
            }
            sections.push_back(std::move(sec));
        }
        // Pre-order: the section chain, then each section's children.
        size_t n_top = sections.size();
        std::vector<int> first_child;
        size_t at = n_top;
        for (const Section& s : sections) {
            first_child.push_back(s.kids.empty() ? -1 : int(at));
            at += s.kids.size();
        }
        Bytes out;
        add32(out, 1);
        add32(out, 0x18);
        out.resize(out.size() + 16, 0);
        put_name(out, 8, prefix);
        for (size_t k = 0; k < n_top; k++) {
            Bytes r0 = sections[k].rec;
            put16(r0, 0x44, uint16_t(k + 1 < n_top ? int(k + 1) : -1));
            put16(r0, 0x46, uint16_t(first_child[k]));
            out.insert(out.end(), r0.begin(), r0.end());
        }
        for (size_t k = 0; k < n_top; k++) {
            const auto& kids = sections[k].kids;
            for (size_t j = 0; j < kids.size(); j++) {
                Bytes kid = kids[j];
                put16(kid, 0x44, uint16_t(j + 1 < kids.size() ? int(first_child[k] + j + 1) : -1));
                put16(kid, 0x46, 0xFFFF);
                out.insert(out.end(), kid.begin(), kid.end());
            }
        }
        return out;
    }

    // ---------------------------------------------------------------------------------------------------------------
    // Collision

    // Rush 2 matrix = B * M^T * Ainv, where render = A * collision space and Rush 2 local = B * Rush 1 local (both A and
    // B take (x, y, z) to (y, -z, x)), and M is Rush 1's stored matrix, which Rush 1 multiplies by transposed
    // (func_80079B90, func_80078414: local = M^T (p - origin)) where Rush 2 multiplies by the matrix itself.
    const int axis_a[3][3] = { { 0, 1, 0 }, { 0, 0, -1 }, { 1, 0, 0 } };
    const int axis_ainv[3][3] = { { 0, 0, 1 }, { 1, 0, 0 }, { 0, -1, 0 } };

    template <typename T>
    void convert_matrix(const T in[3][3], T out[3][3]) {
        T bm[3][3];
        for (int i = 0; i < 3; i++) {
            for (int j = 0; j < 3; j++) {
                T acc = 0;
                for (int k = 0; k < 3; k++) acc = acc + T(axis_a[i][k]) * in[k][j];
                bm[i][j] = acc;
            }
        }
        for (int i = 0; i < 3; i++) {
            for (int j = 0; j < 3; j++) {
                T acc = 0;
                for (int k = 0; k < 3; k++) acc = acc + bm[i][k] * T(axis_ainv[k][j]);
                out[i][j] = acc;
            }
        }
    }

    Bytes convert_collision(const Bytes& d) {
        if (d.size() < 0x20) fail("collision too small");
        uint16_t h[16];
        for (int i = 0; i < 16; i++) h[i] = u16(d, i * 2);
        uint32_t ns = h[0], nn = h[1], np = h[2], nv = h[3], lb = h[4], vb = h[5], wrap = h[14];
        size_t o = 0x20;
        size_t segs = o; o += ns * 0x84;
        size_t nodes = o; o += nn * 0x14;
        size_t polys = o; o += np * 0x1A;
        size_t verts = o; o += nv * 8;
        size_t leaf = o; o += lb;
        size_t vlist = o; o += vb;
        if (o != d.size()) fail("collision size mismatch");
        if (wrap >= ns && wrap != 0) fail("segment wrap out of range");

        Bytes out;
        uint32_t n_segs = ns + (wrap ? 1 : 0);
        if (n_segs > 0xFFFF) fail("collision too large");
        add16(out, uint16_t(n_segs)); add16(out, uint16_t(nn)); add16(out, uint16_t(np));
        add16(out, uint16_t(nv)); add16(out, uint16_t(lb)); add16(out, uint16_t(vb));
        // Segments: origin and the lerped vectors are world points/directions; the matrix maps world to segment frame.
        // Rush 1 wraps the last segment to `wrap`, Rush 2 to 0: a copy of `wrap` follows the last one.
        for (uint32_t s = 0; s < n_segs; s++) {
            size_t so = segs + (s < ns ? s : wrap) * 0x84;
            double f[33];
            for (int k = 0; k < 33; k++) f[k] = f32(d, so + k * 4);
            double g[33];
            std::copy(f, f + 33, g);
            auto to_render = [&](int at) { g[at] = f[at + 1]; g[at + 1] = -f[at + 2]; g[at + 2] = f[at]; };
            to_render(0);
            double m[3][3], m2[3][3];
            for (int i = 0; i < 9; i++) m[i % 3][i / 3] = f[3 + i];   // Rush 1 stores it transposed (func_80078414)
            convert_matrix(m, m2);
            for (int i = 0; i < 9; i++) g[3 + i] = m2[i / 3][i % 3];
            to_render(12); to_render(15); to_render(18);
            for (int k = 0; k < 33; k++) addf(out, g[k]);
        }
        out.insert(out.end(), d.begin() + nodes, d.begin() + nodes + nn * 0x14);
        // Polygons and vertices. A polygon's first vertex is its world origin, the others are in its frame; every
        // vertex is one or the other, and both kinds take (x, y, z) to (y, -z, x).
        std::vector<bool> local(nv, false), origin(nv, false);
        for (uint32_t i = 0; i < np; i++) {
            size_t p = polys + i * 0x1A;
            uint16_t flags = u16(d, p), info = u16(d, p + 2), voff = u16(d, p + 0x18);
            int m[3][3], m2[3][3];
            for (int k = 0; k < 9; k++) m[k % 3][k / 3] = s16(d, p + 6 + k * 2);   // transposed (func_80079B90)
            convert_matrix(m, m2);
            add16(out, flags);
            add16(out, info);
            for (int k = 0; k < 9; k++) add16(out, uint16_t(int16_t(m2[k / 3][k % 3])));
            add16(out, voff);
            // Vertex list: u16 first index, then (if more remain) an optional run byte >= 0xC0.
            size_t at = vlist + voff;
            int count = info & 0xF;
            bool first = true;
            while (count > 0) {
                if (at + 2 > vlist + vb) fail("vertex list past the end");
                uint32_t v = u16(d, at);
                at += 2;
                int run = 0;
                if (count >= 2 && at < vlist + vb && d[at] >= 0xC0) run = d[at++] & 0x3F;
                count -= run + 1;
                for (int k = 0; k <= run; k++) {
                    if (v + k >= nv) fail("vertex index out of range");
                    (first ? origin : local)[v + k] = true;
                    first = false;
                }
            }
        }
        for (uint32_t k = 0; k < nv; k++) {
            if (local[k] && origin[k]) fail("vertex " + std::to_string(k) + " is both a polygon origin and a local vertex");
        }
        for (uint32_t k = 0; k < nv; k++) {
            size_t v = verts + k * 8;
            uint16_t frac = u16(d, v + 6);
            int32_t x = int32_t(s16(d, v)) * 32 + ((frac >> 10) & 31);
            int32_t y = int32_t(s16(d, v + 2)) * 32 + ((frac >> 5) & 31);
            int32_t z = int32_t(s16(d, v + 4)) * 32 + (frac & 31);
            int32_t n[3] = { y, -z, x };
            for (int a = 0; a < 3; a++) {
                int32_t i = n[a] >> 5;
                if (i < -32768 || i > 32767) fail("collision vertex out of range");
                add16(out, uint16_t(int16_t(i)));
            }
            add16(out, uint16_t(((n[0] & 31) << 10) | ((n[1] & 31) << 5) | (n[2] & 31)));
        }
        out.insert(out.end(), d.begin() + leaf, d.begin() + leaf + lb);
        out.insert(out.end(), d.begin() + vlist, d.begin() + vlist + vb);
        return out;
    }

    // ---------------------------------------------------------------------------------------------------------------
    // AI path

    struct LanePoint {
        int x, y, z;
        uint8_t speed, behaviour;
    };

    struct Checkpoint {
        double pos[3];
        int flags;
    };

    struct Gate {
        double pos[3];
        double dir[3];
        uint32_t r2;
        int flags;
    };

    double dist2(const LanePoint& p, const double* q) {
        double dx = p.x - q[0], dy = p.y - q[1], dz = p.z - q[2];
        return dx * dx + dy * dy + dz * dz;
    }

    // func_80092BC4 / func_80092884 (tools/rush2049/paths.py crossing): the first index where a path changes sides of
    // the gate plane within the radius, else the closest point.
    int crossing(const Gate& g, const std::vector<LanePoint>& pts) {
        size_t n = pts.size();
        double best = 3.4e38;
        int best_i = 0, prev_side = 0;
        double prev_d2 = 0.0;
        long k = -1;
        size_t i = 0;
        while (true) {
            if (i == n) i = 0;
            double dx = pts[i].x - g.pos[0], dz = pts[i].z - g.pos[2];
            int side = dz * g.dir[2] + dx * g.dir[0] < 0 ? -1 : 1;
            double d2 = dx * dx + dz * dz;
            if (d2 < best) { best = d2; best_i = int(i); }
            if (k >= 0 && d2 <= g.r2 && prev_d2 <= g.r2 && side != prev_side) return int(i);
            k++; i++; prev_d2 = d2; prev_side = side;
            if (k >= long(n)) return best_i;
        }
    }

    Bytes convert_path(const std::vector<uint8_t>& rom, const Main& main, int t, bool backward,
                       std::vector<int16_t>& demo, float& lap_seconds,
                       rush2::track1::ConvertedTrack::Timing& timing) {
        Bytes d = asset(rom, (backward ? path_back_asset : path_asset) + t);
        std::vector<std::vector<LanePoint>> lanes;
        size_t o = 0;
        for (int l = 0; l < 4; l++) {
            int n = s16(d, o), end = s16(d, o + 4);
            if (n < 0) fail("bad lane");
            std::vector<LanePoint> pts;
            for (int k = 0; k < n; k++) {
                size_t p = o + 8 + size_t(k) * 10;
                int c0 = s16(d, p), c1 = s16(d, p + 2), c2 = s16(d, p + 4);
                pts.push_back({ c1, -c2, c0, d[p + 6], d[p + 7] });
            }
            o += 8 + size_t(n) * 10;
            if (!(2 <= end && end <= n)) end = n;
            pts.resize(size_t(end));
            lanes.push_back(std::move(pts));
        }
        if (o != d.size()) fail("path size mismatch");

        // Records: f32 x, y, z, f32, s16 flags, s16 time[3] (by lap); the end record (flags -1) holds the start time's
        // base at +0x12 and the loop-start checkpoint at +0x14.
        std::vector<Checkpoint> cps;
        int loop_cp = 0;
        timing.checkpoints.clear();
        uint32_t list = main.w(checkpoint_lists[backward ? 1 : 0] + t * 4);
        for (int i = 0;; i++) {
            if (i >= 13) fail("checkpoint list has no end");
            uint32_t r = list + i * 0x18;
            int flags = int16_t(main.w(r + 16) >> 16);
            if (flags == -1) {
                timing.start = int16_t(main.w(r + 16));
                loop_cp = int16_t(main.w(r + 20) >> 16);
                break;
            }
            timing.checkpoints.push_back({ int16_t(main.w(r + 16)), int16_t(main.w(r + 20) >> 16),
                                           int16_t(main.w(r + 20)) });
            uint32_t bits[3] = { main.w(r), main.w(r + 4), main.w(r + 8) };
            Checkpoint c{};
            for (int a = 0; a < 3; a++) {
                float f;
                memcpy(&f, &bits[a], 4);
                c.pos[a] = f;
            }
            c.flags = flags;
            cps.push_back(c);
        }
        if (cps.empty() || cps.size() > 10) fail(std::to_string(cps.size()) + " checkpoints");

        const std::vector<LanePoint>& spine = lanes[0];
        size_t n = spine.size();
        auto nearest = [](const std::vector<LanePoint>& pts, const double* pos, size_t lo, size_t hi) {
            hi = std::max(hi, lo + 1);
            size_t best = lo;
            for (size_t j = lo; j < hi; j++) {
                if (j >= pts.size()) fail("checkpoint search past the end of a lane");
                if (dist2(pts[j], pos) < dist2(pts[best], pos)) best = j;
            }
            return best;
        };
        // Checkpoints in race order: each one's nearest spine and lane points after the previous checkpoint's.
        std::vector<size_t> ks;
        std::vector<std::array<size_t, 4>> es;
        for (size_t i = 0; i < cps.size(); i++) {
            ks.push_back(nearest(spine, cps[i].pos, i == 0 ? 0 : ks.back() + 1, i == 0 ? n / 2 : n));
            std::array<size_t, 4> e{};
            for (int l = 0; l < 4; l++) {
                e[l] = nearest(lanes[l], cps[i].pos, i == 0 ? 0 : es.back()[l] + 1,
                               i == 0 ? lanes[l].size() / 2 : lanes[l].size());
            }
            es.push_back(e);
        }
        // Gates on spine points, as wide as their lanes need; a gate moves along the spine until every path's crossing
        // (as Rush 2 finds it) is the intended one.
        std::vector<Gate> gates;
        for (size_t i = 0; i < cps.size(); i++) {
            int flags = (i == 0 ? 2 : 0) | (int(i) == loop_cp ? 4 : 0) | ((cps[i].flags & 2) ? 1 : 0);
            std::optional<Gate> found;
            for (int step = 0; step <= 40 && !found; step++) {
                long kk = long(ks[i]) + ((step + 1) / 2) * (step % 2 ? 1 : -1);
                if (kk < 0 || kk >= long(n)) continue;
                const LanePoint& a = spine[size_t(std::max(kk - 2, 0L))];
                const LanePoint& b = spine[size_t(std::min(kk + 2, long(n) - 1))];
                double dx = b.x - a.x, dz = b.z - a.z;
                double len = std::sqrt(dx * dx + dz * dz);
                if (len == 0) len = 1.0;
                Gate g{ { double(spine[size_t(kk)].x), double(spine[size_t(kk)].y), double(spine[size_t(kk)].z) },
                        { dx / len, 0.0, dz / len }, 0, flags };
                double rad = 60.0;
                for (int l = 0; l < 4; l++) {
                    const auto& pts = lanes[l];
                    long e = long(es[i][l]);
                    std::optional<double> best;
                    for (long j = std::max(e - 40, 0L); j < std::min(e + 40, long(pts.size())); j++) {
                        const LanePoint& p = pts[size_t(j)];
                        const LanePoint& q = pts[size_t(j + 1) % pts.size()];
                        double sp = (p.x - g.pos[0]) * g.dir[0] + (p.z - g.pos[2]) * g.dir[2];
                        double sq = (q.x - g.pos[0]) * g.dir[0] + (q.z - g.pos[2]) * g.dir[2];
                        if ((sp < 0) != (sq < 0)) {
                            double px = p.x - g.pos[0], pz = p.z - g.pos[2], qx = q.x - g.pos[0], qz = q.z - g.pos[2];
                            double dd = std::max(std::sqrt(px * px + pz * pz), std::sqrt(qx * qx + qz * qz));
                            best = best ? std::min(*best, dd) : dd;
                        }
                    }
                    if (best) rad = std::max(rad, *best + 10);
                }
                rad = std::min(rad, 600.0);
                g.r2 = uint32_t(rad * rad);
                bool ok = std::abs(crossing(g, spine) - kk) <= 40;
                for (int l = 0; l < 4 && ok; l++) ok = std::abs(crossing(g, lanes[l]) - long(es[i][l])) <= 40;
                if (ok) found = g;
            }
            if (!found) fail("no gate for checkpoint " + std::to_string(i));
            gates.push_back(*found);
        }

        // Rush 2's path file (tools/rush2049/paths.py build, runtime fields cleared).
        Bytes out(0x32C, 0);
        put16(out, 0, 90);
        put16(out, 2, 0xFFFF); put16(out, 4, 0xFFFF); put16(out, 6, 0xFFFF);
        put16(out, 8, uint16_t(gates.size()));
        for (size_t i = 0; i < gates.size(); i++) {
            size_t c = 0xC + i * 0x50;
            for (int a = 0; a < 3; a++) putf(out, c + a * 4, gates[i].pos[a]);
            for (int a = 0; a < 3; a++) putf(out, c + 0xC + a * 4, gates[i].dir[a]);
            put32(out, c + 0x18, gates[i].r2);
            put16(out, c + 0x1C, uint16_t(gates[i].flags));
            put16(out, c + 0x1E, 45);
            put16(out, c + 0x20, 45);
            for (int k = 0; k < 20; k++) put16(out, c + 0x22 + k * 2, 0xFFFF);
        }
        Bytes route(16, 0);
        put16(route, 0, uint16_t(n));
        out.insert(out.end(), route.begin(), route.end());
        add16(out, uint16_t(n));
        for (const LanePoint& p : spine) {
            add16(out, uint16_t(int16_t(p.x))); add16(out, uint16_t(int16_t(p.y))); add16(out, uint16_t(int16_t(p.z)));
        }
        for (const auto& pts : lanes) {
            add16(out, uint16_t(pts.size()));
            out.push_back(0); out.push_back(0);
            add32(out, 0);
            for (const LanePoint& p : pts) {
                add16(out, uint16_t(int16_t(p.x))); add16(out, uint16_t(int16_t(p.y))); add16(out, uint16_t(int16_t(p.z)));
                out.push_back(p.speed);
                out.push_back(p.behaviour);
            }
        }
        demo = { 0, int16_t(n / 4), int16_t(n / 2), int16_t(3 * n / 4) };

        // The AI's lap time on lane 1, at the lanes' target speeds (mph).
        double seconds = 0.0;
        const auto& racing = lanes[1];
        for (size_t j = 0; j + 1 < racing.size(); j++) {
            double dx = racing[j + 1].x - racing[j].x, dy = racing[j + 1].y - racing[j].y, dz = racing[j + 1].z - racing[j].z;
            double fps = racing[j].speed * 88.0 / 60.0;
            if (fps > 0) seconds += std::sqrt(dx * dx + dy * dy + dz * dz) / fps;
        }
        lap_seconds = float(seconds);
        return out;
    }
}

bool rush2::track1::convert_track(const std::vector<uint8_t>& rom, int t, const std::string& prefix,
                                  ConvertedTrack& out, std::string& error) {
    if (t < 0 || t >= track_count) {
        error = "no such Rush 1 track";
        return false;
    }
    auto main_data = main_code(rom);
    if (main_data == nullptr) {
        error = "can't read the Rush 1 main code";
        return false;
    }
    try {
        Main main{ *main_data };
        std::map<std::string, std::string> names;
        out.geometry = convert_geometry(rom, main, t, prefix, names);
        out.placement = convert_placement(rom, t, prefix, names);
        out.collision[0] = convert_collision(asset(rom, collision_asset + t));
        out.collision[1] = convert_collision(asset(rom, collision_back_asset + t));
        for (int b = 0; b < 2; b++) {
            out.path[b] = convert_path(rom, main, t, b != 0, out.demo_starts[b], out.lap_seconds[b], out.timing[b]);
        }
        out.pvs_count = main.b(pvs_counts + t);
        out.pvs.clear();
        for (uint32_t i = 0; i < uint32_t(out.pvs_count) * 16; i++) {
            out.pvs.push_back(main.b(pvs_tables[t] + i));
        }
    }
    catch (const ConvertError& e) {
        error = e.message;
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------------------------------------------------
// Track select art. Rush 1's track select shows each track as a 3D model (asset 10: TRK1O2, TRK_2O2, ...), much like
// Rush 2's dioramas, so those models are used as they are (flattened and translated, every call inlined). The logos
// are made here: Rush 1 shows its track names with its own models and fonts, which don't fit Rush 2's 128x32 logo
// slot, so each logo is the track's route seen from above and "TRACK n".

namespace {
    const char* const diorama_models[rush2::track1::track_count] = { "TRK1O2", "TRK_2O2", "TRK_3O2", "TRK4O2", "TRK5O1",
                                                                     "TRK6O3", "TRK7O5" };
    constexpr int logo_w = 128, logo_h = 32;
    constexpr float diorama_size = 35.0f;   // Radius x scale of the stock dioramas, on average.

    // 5x7 capitals and digits, one byte per row, bit 4 = leftmost column.
    const std::map<char, std::array<uint8_t, 7>> font = {
        { 'A', { 0x0E, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11 } }, { 'C', { 0x0E, 0x11, 0x10, 0x10, 0x10, 0x11, 0x0E } },
        { 'F', { 0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x10 } }, { 'H', { 0x11, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11 } },
        { 'K', { 0x11, 0x12, 0x14, 0x18, 0x14, 0x12, 0x11 } }, { 'R', { 0x1E, 0x11, 0x11, 0x1E, 0x14, 0x12, 0x11 } },
        { 'S', { 0x0F, 0x10, 0x10, 0x0E, 0x01, 0x01, 0x1E } }, { 'T', { 0x1F, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04 } },
        { 'U', { 0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E } }, { '1', { 0x04, 0x0C, 0x04, 0x04, 0x04, 0x04, 0x0E } },
        { '2', { 0x0E, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1F } }, { '3', { 0x1F, 0x02, 0x04, 0x02, 0x01, 0x11, 0x0E } },
        { '4', { 0x02, 0x06, 0x0A, 0x12, 0x1F, 0x02, 0x02 } }, { '5', { 0x1F, 0x10, 0x1E, 0x01, 0x01, 0x11, 0x0E } },
        { '6', { 0x06, 0x08, 0x10, 0x1E, 0x11, 0x11, 0x0E } }, { '7', { 0x1F, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08 } },
    };

    // Palette: 0 transparent, then the text and route inks.
    constexpr uint8_t ink_big = 1, ink_small = 2, ink_outline = 3, ink_route = 4;

    uint16_t rgba5551(int r, int g, int b, int a) {
        return uint16_t(((r >> 3) << 11) | ((g >> 3) << 6) | ((b >> 3) << 1) | (a ? 1 : 0));
    }

    void draw_text(std::array<uint8_t, logo_w * logo_h>& img, const std::string& text, int x0, int y0, int scale, uint8_t ink) {
        int x = x0;
        for (char ch : text) {
            auto g = font.find(ch);
            if (g != font.end()) {
                for (int row = 0; row < 7; row++) {
                    for (int col = 0; col < 5; col++) {
                        if (!(g->second[size_t(row)] & (0x10 >> col))) continue;
                        for (int sy = 0; sy < scale; sy++) {
                            for (int sx = 0; sx < scale; sx++) {
                                int px = x + col * scale + sx, py = y0 + row * scale + sy;
                                if (px >= 0 && px < logo_w && py >= 0 && py < logo_h) img[size_t(py * logo_w + px)] = ink;
                            }
                        }
                    }
                }
            }
            x += 6 * scale;
        }
    }

    // Track t's route (lane 0 of the forward lanes, render x/z), or empty.
    std::vector<std::pair<int, int>> route(const std::vector<uint8_t>& rom, int t) {
        std::vector<std::pair<int, int>> out;
        Bytes d;
        if (!read(rom, path_asset + t, d) || d.size() < 8) return out;
        int n = int16_t(u16(d, 0)), end = int16_t(u16(d, 4));
        if (n < 2 || 8 + size_t(n) * 10 > d.size()) return out;
        if (!(2 <= end && end <= n)) end = n;
        for (int k = 0; k < end; k++) {
            size_t p = 8 + size_t(k) * 10;
            out.push_back({ s16(d, p + 2), s16(d, p) });   // render x = collision c1, render z = collision c0
        }
        return out;
    }

    // 128x32 CI8 texels (rows bottom-up, as Rush 2 stores its logos) and a 256-entry RGBA5551 palette.
    void build_logo(const std::vector<uint8_t>& rom, int t, Bytes& texels, Bytes& palette) {
        std::array<uint16_t, 256> pal{};
        pal[ink_big] = rgba5551(255, 196, 24, 1);
        pal[ink_small] = rgba5551(235, 235, 245, 1);
        pal[ink_outline] = rgba5551(24, 10, 0, 1);
        pal[ink_route] = rgba5551(232, 32, 24, 1);

        std::array<uint8_t, logo_w * logo_h> img{};
        // Icon: the route from above, fitted into 28x28 at (2, 2), north up.
        auto pts = route(rom, t);
        if (!pts.empty()) {
            int x0 = pts[0].first, x1 = x0, z0 = pts[0].second, z1 = z0;
            for (auto [x, z] : pts) { x0 = std::min(x0, x); x1 = std::max(x1, x); z0 = std::min(z0, z); z1 = std::max(z1, z); }
            float scale = 27.0f / float(std::max({ x1 - x0, z1 - z0, 1 }));
            float ox = 2 + (27 - (x1 - x0) * scale) / 2, oy = 2 + (27 - (z1 - z0) * scale) / 2;
            for (size_t k = 0; k < pts.size(); k++) {
                auto [ax, az] = pts[k];
                auto [bx, bz] = pts[(k + 1) % pts.size()];
                for (int s = 0; s <= 8; s++) {
                    float x = ax + (bx - ax) * s / 8.0f, z = az + (bz - az) * s / 8.0f;
                    int px = int(ox + (x - x0) * scale), py = int(oy + (z1 - z) * scale);
                    for (int dy = 0; dy <= 1; dy++) for (int dx = 0; dx <= 1; dx++) {
                        int qx = px + dx, qy = py + dy;
                        if (qx >= 0 && qx < 32 && qy >= 0 && qy < logo_h) img[size_t(qy * logo_w + qx)] = ink_route;
                    }
                }
            }
        }
        // Name, with a one-pixel outline around the text.
        std::array<uint8_t, logo_w * logo_h> text{};
        draw_text(text, "SF RUSH", 38, 2, 1, ink_small);
        draw_text(text, "TRACK " + std::to_string(t + 1), 36, 13, 2, ink_big);
        for (int y = 0; y < logo_h; y++) {
            for (int x = 34; x < logo_w; x++) {
                if (text[size_t(y * logo_w + x)]) {
                    img[size_t(y * logo_w + x)] = text[size_t(y * logo_w + x)];
                    continue;
                }
                bool near_ink = false;
                for (int dy = -1; dy <= 1; dy++) for (int dx = -1; dx <= 1; dx++) {
                    int px = x + dx, py = y + dy;
                    if (px >= 0 && px < logo_w && py >= 0 && py < logo_h && text[size_t(py * logo_w + px)]) near_ink = true;
                }
                if (near_ink) img[size_t(y * logo_w + x)] = ink_outline;
            }
        }
        texels.assign(logo_w * logo_h, 0);
        for (int y = 0; y < logo_h; y++) {
            memcpy(&texels[size_t((logo_h - 1 - y) * logo_w)], &img[size_t(y * logo_w)], logo_w);
        }
        palette.clear();
        for (uint16_t c : pal) add16(palette, c);
    }

    std::optional<uint32_t> find_model(const Container& c, const std::string& name) {
        for (uint32_t i = 0; i < c.n_models; i++) {
            if (c.name(i) == name) return i;
        }
        return std::nullopt;
    }
}

bool rush2::track1::extend_menu_container(const std::vector<uint8_t>& container, const std::vector<uint8_t>& rom,
                                          std::vector<uint8_t>& out) {
    try {
        Container dioramas(asset(rom, diorama_asset));
        Builder b;
        b.extend(container);
        int s = b.add_source(dioramas);
        for (int t = 0; t < track_count; t++) {
            auto i = find_model(dioramas, diorama_models[t]);
            if (!i) fail(std::string("no diorama ") + diorama_models[t]);
            b.add_model(s, *i, "R1TRACK" + std::to_string(t + 1), false);
            Bytes texels, palette;
            build_logo(rom, t, texels, palette);
            b.add_texels("R1LOGO" + std::to_string(t + 1), logo_w, logo_h, 0x01, 0x02, 0x48008000, texels, palette);
        }
        out = b.build();
    }
    catch (const ConvertError& e) {
        printf("[Rush1] Couldn't build the track select art: %s\n", e.message.c_str());
        return false;
    }
    return true;
}

float rush2::track1::diorama_scale(const std::vector<uint8_t>& rom, int t) {
    static std::mutex scale_mutex;
    static const std::vector<uint8_t>* cached_rom = nullptr;
    static std::array<float, track_count> scales{};
    std::lock_guard lock{ scale_mutex };
    if (cached_rom != &rom) {
        cached_rom = &rom;
        scales.fill(1.0f);
        try {
            Container dioramas(asset(rom, diorama_asset));
            for (int k = 0; k < track_count; k++) {
                auto i = find_model(dioramas, diorama_models[k]);
                float radius = i ? dioramas.radius(*i) : 0.0f;
                if (radius > 1.0f) scales[size_t(k)] = diorama_size / radius;
            }
        }
        catch (const ConvertError&) {
        }
    }
    return t >= 0 && t < track_count ? scales[size_t(t)] : 1.0f;
}

bool rush2::track1::build_race_logo(const std::vector<uint8_t>& logo, const std::vector<uint8_t>& rom, int t,
                                    std::vector<uint8_t>& out) {
    // Rush 2's logo containers hold one texture and its palette; overwrite their data in place.
    try {
        if (logo.size() < 0x28 || u32(logo, 20) != 1 || u32(logo, 24) != 1) return false;
        uint32_t tex = u32(logo, 8), pal = u32(logo, 12);
        uint32_t texels = u32(logo, tex + 24), palette = u32(logo, pal + 20);
        if (u16(logo, tex + 16) != logo_w || u16(logo, tex + 18) != logo_h || texels + logo_w * logo_h > logo.size() ||
            palette + 512 > logo.size()) {
            return false;
        }
        Bytes t_bytes, p_bytes;
        build_logo(rom, t, t_bytes, p_bytes);
        out = logo;
        memcpy(&out[texels], t_bytes.data(), t_bytes.size());
        memcpy(&out[palette], p_bytes.data(), p_bytes.size());
    }
    catch (const ConvertError&) {
        return false;
    }
    return true;
}
