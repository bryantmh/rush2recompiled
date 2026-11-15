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
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "clipper2/clipper.h"

#include "diorama_look.h"
#include "snowfall.h"
#include "track1.h"
#include "track1_banners.h"
#include "track1_lanes.h"
#include "track2049.h"
#include "texture_upscale.h"
#include "path_lane_fix.h"

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

    // Rush 1 breakables raced as Rush 2 breakable classes, drawn with Rush 1's models. A record keeps its model name when
    // that already selects the Rush 2 class (CONE1L1 -> CONE1), else its prefix is replaced to select one; the class
    // gives the sound and debris handling, and the model's name record gets the behaviour id of the Rush 2 model it
    // stands in for (src/rush1/track1.cpp redirects Rush 2's lookups of the class model to the record's own model).
    struct ObjectClass {
        const char* prefix;     // Rush 1 model name prefix.
        const char* record;     // Replacement for the prefix in the record name, or null to keep it.
        int16_t kind;           // Behaviour id of the Rush 2 class model (CONE1O1, METERO1, TREEHIT1O1, ...).
    };
    const ObjectClass r1_classes[] = { { "CONE1L", nullptr, 2 }, { "METERL", nullptr, 5 }, { "TREEHIT", nullptr, 5 },
        { "FLAG2L", nullptr, 23 }, { "FENCEL", nullptr, 4 }, { "GASIGNL", nullptr, 7 }, { "WINDOWBL", "SHATPANEBL", 9 },
        { "PMUNCH_01L", "CURVEHITPMUNCH", 5 }, { "TMUNCHL", "TREEHITTMUNCH", 5 }, { "T5GATEL", "FENCET5GATEL", 4 } };
    // Rush 2's breakable pieces (resolved by name at race start, func_8008A1FC) -> Rush 1's: {Rush 2 prefix, Rush 1
    // prefix, Rush 2 numbers, offset to Rush 1's number}.
    struct PieceSet {
        const char* rush2;
        const char* rush1;
        int count;
        int shift;
    };
    const PieceSet r1_pieces[] = { { "CONE1O", "CONE1L", 1, 0 }, { "METERO", "METERL", 1, 0 },
        { "SHATPANEO", "WINDOWBL", 7, 0 }, { "FENCEO", "FENCEL", 12, 0 }, { "FLAG2O", "FLAG2L", 10, -1 },
        { "GASIGNO", "GASIGNL", 3, 0 }, { "T5GATEO", "T5GATEL", 12, 0 } };
    // A fence record with +0x4A set is track 5's gate: Rush 1's fence spawner (0x800C32EC) gives it model T5GATEL1 and
    // pieces T5GATEL1-12 instead of FENCEL1-12 (src/rush1/track1.cpp gives the Rush 2 fence the T5GATEO piece ids).
    constexpr const char* r1_fence = "FENCEL";
    constexpr const char* r1_gate = "T5GATEL1";
    // Tree pieces TREEHITnO1 -> TREEHITnL1 (n 1-4).
    // Rush 1 models from the shared object file (asset 12) that the classes and keys use.
    const char* const r1_shared_objects[] = { "CONE1L1", "METERL1", "TREEHIT1L1", "TREEHIT4L1", "KEYL1" };
    // Keys (KEYL1, key number 1-8 at record +0x4A) become Rush 2 key records KEY1-8 (the KEY class, behaviour 8) drawn
    // with Rush 1's key model; src/collectibles.cpp gives key n bit n - 1.
    constexpr const char* r1_key = "KEYL";
    constexpr const char* r1_key_model = "KEYL1";
    constexpr int r1_keys_max = 8;
    constexpr int16_t key_behaviour = 8;

    // Rush 1 placement objects that are left out (MARKER and TIME are unknown; BIGCHEER2 is silent in Rush 1).
    const char* const r1_dropped[] = { "MARKER", "TIME", "BIGCHEER2" };
    // Rush 1 sound emitters -> the Rush 2 emitters that play the same sound at the same range (the behaviour ids differ:
    // SMALLHOOT's is Rush 2's BIGCHR1 and BIGCHEER's BIGCHR2, a crowd cheer at 400 and 300). FIRECRK's fireworks sound
    // is Rush 1's own (src/rush1/track1_audio.cpp). BIGCHEER2's behaviour (id 0) does nothing, so it is left out.
    const std::pair<const char*, const char*> r1_emitters[] = { { "CCAR", "CABLECAR" }, { "FIRECRK", "FIRECRCK" },
        { "SMALLHOOT", "BIGCHR1" }, { "BIGCHEER", "BIGCHR2" } };

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
            n_tex = u32(d, 20);
            tex_at = n_tex ? off(u32(d, 16)) : 0;   // The cars have no texture table (pointer 0).
            n_pal = u32(d, 28);
            pal_at = n_pal ? off(u32(d, 24)) : 0;
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
        std::string model = {};   // Set: the start of that model's first list (a call into it), the rest unused.
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
            int16_t kind = 0;   // Behaviour id (name record +0x14).
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

        // Raw data (vertices of the converter's own) placed in the container, as a source without a container: Refs
        // { false, index, offset } point into it.
        int add_raw(const Bytes& data) {
            align8(out);
            sources.push_back({ nullptr, uint32_t(out.size()) });
            out.insert(out.end(), data.begin(), data.end());
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

        // The extended container: the new model lists, then every table rebuilt (old and new records, sorted). Models
        // whose lists call another model's (Ref::model) are placed after the others.
        std::map<std::string, uint32_t> placed_lists;
        Bytes build_extended() {
            if (!load_lists.empty() || !textures.empty() || !palettes.empty()) fail("extension with texture-load lists");
            align8(out);
            std::vector<uint32_t> no_loads;
            auto calls = [](const ModelOut& m) {
                for (const auto& lod : m.lods) {
                    if (lod) for (const Fixup& f : lod->fixups) if (!f.ref.model.empty()) return true;
                }
                return false;
            };
            std::stable_partition(models.begin(), models.end(), [&](const ModelOut& m) { return !calls(m); });
            for (ModelOut& m : models) {
                Bytes rec, name_rec(0x18, 0);
                add32(rec, m.count);
                for (int l = 0; l < 4; l++) {
                    uint32_t at = 0;
                    if (m.lods[l]) {
                        at = uint32_t(out.size());
                        out.insert(out.end(), m.lods[l]->bytes.begin(), m.lods[l]->bytes.end());
                        for (const Fixup& f : m.lods[l]->fixups) put32(out, at + f.pos, resolve(f.ref, no_loads));
                        if (l == 0) placed_lists[m.name] = at;
                    }
                    add16(rec, 0);
                    add16(rec, m.flags[l]);
                    addf(rec, m.dist[l]);
                    add32(rec, at);
                }
                put_name(name_rec, 0, m.name);
                putf(name_rec, 16, m.radius);
                put16(name_rec, 20, uint16_t(m.kind));
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

        // The output offsets of the vertices model m loads (G_VTX fixups into the sources' copies).
        std::set<size_t> vertices_of(const ModelOut& m) const {
            std::set<size_t> at;
            for (const auto& lod : m.lods) {
                if (!lod) continue;
                for (const Fixup& f : lod->fixups) {
                    if (f.ref.load || f.pos < 4 || lod->bytes[f.pos - 4] != 0x01) continue;
                    uint32_t n = (u32(lod->bytes, f.pos - 4) >> 12) & 0xFF;
                    for (uint32_t k = 0; k < n; k++) {
                        at.insert(sources[size_t(f.ref.index)].second + f.ref.offset + k * 16);
                    }
                }
            }
            return at;
        }

        // Moves the models `family` (placed models with their animation frames and pieces, which share their origin) up
        // so that the lowest vertex of the `placed` ones is at y 0, as Rush 2's breakable models are built (their
        // placement point is their base); returns the shift in vertex units (1/16 of a world unit).
        int base_origin(const std::set<std::string>& placed, const std::set<std::string>& family) {
            std::set<size_t> mine, base;
            for (const std::string& name : family) {
                const ModelOut* model = nullptr;
                for (const ModelOut& m : models) {
                    if (m.name == name) model = &m;
                }
                if (model == nullptr) fail("no model " + name + " to move");
                std::set<size_t> v = vertices_of(*model);
                mine.insert(v.begin(), v.end());
                if (placed.count(name)) base.insert(v.begin(), v.end());
            }
            if (base.empty()) return 0;
            for (const ModelOut& m : models) {
                if (family.count(m.name)) continue;
                for (size_t a : vertices_of(m)) {
                    if (mine.count(a)) fail("model " + m.name + " shares vertices with " + *family.begin());
                }
            }
            int lowest = 0x7FFF;
            for (size_t a : base) lowest = std::min(lowest, int(s16(out, a + 2)));
            int shift = -lowest;
            for (size_t a : mine) put16(out, a + 2, uint16_t(int16_t(s16(out, a + 2) + shift)));
            return shift;
        }

        uint32_t resolve(const Ref& ref, const std::vector<uint32_t>& load_at) const {
            if (!ref.model.empty()) {
                auto it = placed_lists.find(ref.model);
                if (it == placed_lists.end()) fail("call to model " + ref.model + " before its list is placed");
                return it->second;
            }
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
                put16(out, o + 20, uint16_t(m.kind));
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

    const ObjectClass* object_class(const std::string& name) {
        for (const ObjectClass& c : r1_classes) {
            if (starts_with(name, c.prefix)) return &c;
        }
        return nullptr;
    }

    // Names of every record in track t's placement.
    std::string object_name(const Bytes& d, size_t o) {
        std::string n = cname(d, o);
        return starts_with(n, r1_fence) && s16(d, o + 0x4A) != 0 ? r1_gate : n;
    }

    std::set<std::string> placed_objects(const std::vector<uint8_t>& rom, int t) {
        Bytes d = asset(rom, placement_asset + t);
        uint32_t base = u32(d, 4);
        std::set<std::string> out;
        for (size_t o = base; o + 0x64 <= d.size(); o += 0x64) out.insert(object_name(d, o));
        return out;
    }

    // ---------------------------------------------------------------------------------------------------------------
    // Night (docs/sfrush_ps1.md#n64-night-versions): an N64 course raced as a NIGHT version, in the PlayStation SF Rush's
    // night look (the N64 game has none). Every track texture is CI (TEXEL0 x SHADE; a few faces SHADE alone), as the
    // dioramas are, so the condition goes on the way the night dioramas' does (dress_look): each TLUT the lists load
    // goes through diorama_look::night in place (the whole container is the night version's), the shade of a vertex only
    // untextured triangles use too, and a textured triangle's shade is kept (darkening both made it near black).

    // Whether combine mode (w0, w1) reads a texel in either cycle's color (TEXEL0 / TEXEL1 as a, b, c or d, or their
    // alpha as c).
    bool combine_textured(uint32_t w0, uint32_t w1) {
        const uint32_t a[2] = { (w0 >> 20) & 15, (w0 >> 5) & 15 }, c[2] = { (w0 >> 15) & 31, w0 & 31 };
        const uint32_t b[2] = { (w1 >> 28) & 15, (w1 >> 24) & 15 }, d[2] = { (w1 >> 15) & 7, (w1 >> 6) & 7 };
        for (int k = 0; k < 2; k++) {
            if (a[k] == 1 || a[k] == 2 || b[k] == 1 || b[k] == 2 || d[k] == 1 || d[k] == 2) return true;
            if (c[k] == 1 || c[k] == 2 || c[k] == 8 || c[k] == 9) return true;
        }
        return false;
    }

    struct NightStats {
        size_t tluts = 0, entries = 0, shades = 0, shared = 0;
        size_t window_textures = 0, windows = 0, lit_calls = 0, calls = 0;
        size_t art_textures = 0;     // textures redrawn in the PS1's night art
        size_t banner_tluts = 0;     // the CHECKPOINT / FINISH banners' TLUTs, kept lit
    };

    // ---- lit windows ----------------------------------------------------------------------------------------------
    // The PlayStation's night sets light some of the buildings' windows. Here a building texture (one drawn mostly on
    // walls, with windows: rectangles of glass, its dark or blue entries) gets two lit copies, each with about half of
    // its windows lit (window light, varied by the glass's own shading) and the rest dark; each place a model binds
    // the texture draws the original (all dark), or one of the copies, by a hash of the model and the place. A copy is a
    // copy of the texture's load list pointing at its own texels (the unlit windows' glass made the texture's darkest
    // other entry) and TLUT (the glass entries window light).

    // A texture-load list's loads: its TLUT (container offset, entries), its texel block (offset, bytes) and its tiles.
    struct LoadInfo {
        size_t tlut = SIZE_MAX, texels = SIZE_MAX;
        uint32_t tlut_n = 0, bytes = 0;
        uint32_t dxt = 0;                                     // the LOADBLOCK's: 0, its odd rows stored word-swapped
        size_t tlut_fix = SIZE_MAX, texel_fix = SIZE_MAX;   // body offsets of the SETTIMGs' address words
        struct Tile { uint32_t fmt = 0, siz = 0, line = 0, tmem = 0, w = 0, h = 0, palette = 0; bool set = false; };
        std::array<Tile, 8> tiles{};
    };

    LoadInfo read_load(const Builder& b, const Body& body) {
        LoadInfo li;
        std::map<size_t, const Ref*> refs;
        for (const Fixup& f : body.fixups) if (!f.ref.load && f.ref.model.empty()) refs[f.pos] = &f.ref;
        size_t timg = SIZE_MAX, timg_fix = SIZE_MAX;
        for (size_t o = 0; o + 8 <= body.bytes.size(); o += 8) {
            uint32_t w0 = u32(body.bytes, o), w1 = u32(body.bytes, o + 4);
            uint8_t op = uint8_t(w0 >> 24);
            if (op == 0xFD) {
                auto it = refs.find(o + 4);
                timg = it != refs.end() ? size_t(b.sources[size_t(it->second->index)].second) + it->second->offset : SIZE_MAX;
                timg_fix = o + 4;
            }
            else if (op == 0xF0) {
                li.tlut = timg, li.tlut_fix = timg_fix, li.tlut_n = ((w1 >> 14) & 0x3FF) + 1;
            }
            else if (op == 0xF3) {
                // LOADBLOCK of 16-bit texels (the lists load CI as 16-bit blocks).
                li.texels = timg, li.texel_fix = timg_fix, li.bytes = (((w1 >> 12) & 0xFFF) + 1) * 2, li.dxt = w1 & 0xFFF;
            }
            else if (op == 0xF5) {
                LoadInfo::Tile& t = li.tiles[(w1 >> 24) & 7];
                t.fmt = (w0 >> 21) & 7, t.siz = (w0 >> 19) & 3, t.line = (w0 >> 9) & 0x1FF, t.tmem = w0 & 0x1FF, t.palette = (w1 >> 20) & 15, t.set = true;
            }
            else if (op == 0xF2) {
                LoadInfo::Tile& t = li.tiles[(w1 >> 24) & 7];
                t.w = ((w1 >> 12) & 0xFFF) / 4 - ((w0 >> 12) & 0xFFF) / 4 + 1;
                t.h = (w1 & 0xFFF) / 4 - (w0 & 0xFFF) / 4 + 1;
            }
        }
        return li;
    }

    // A texture's levels as the lists draw them: level 0 is the render tile 0, then the mipmaps' tiles in TMEM order.
    // at: byte offset in the texel block. swap: the block was loaded with a LOADBLOCK of dxt 0, which copies it into
    // TMEM as it is, so its odd rows are stored with their 32-bit words swapped (the RDP swaps them back as it samples
    // an odd row); reading such a row straight scrambled half of every texture (the asset dump reads them right).
    struct Level { uint32_t at = 0, w = 0, h = 0, bits = 0; bool swap = false; };

    std::vector<Level> levels(const LoadInfo& li) {
        std::vector<Level> out;
        const LoadInfo::Tile& t0 = li.tiles[0];
        if (!t0.set || t0.fmt != 2 || t0.siz > 1 || t0.line == 0) return out;
        const uint32_t bits = 4u << t0.siz;
        uint32_t w = t0.line * 64 / bits;
        uint32_t h = t0.h ? t0.h : (li.bytes * 8 / bits) / w;
        // The block's place in TMEM is the load tile's (7); a tile's texels are at its TMEM address past that.
        const uint32_t base = li.tiles[7].set ? li.tiles[7].tmem : 0;
        if (t0.tmem < base) return out;
        out.push_back({ (t0.tmem - base) * 8, w, h, bits, li.dxt == 0 });
        std::vector<const LoadInfo::Tile*> mips;
        for (int k = 1; k < 8; k++) {
            const LoadInfo::Tile& t = li.tiles[size_t(k)];
            if (t.set && t.fmt == 2 && t.siz == t0.siz && t.line && t.h && t.tmem > t0.tmem && k < 7) mips.push_back(&t);
        }
        std::sort(mips.begin(), mips.end(), [](const LoadInfo::Tile* a, const LoadInfo::Tile* b) { return a->tmem < b->tmem; });
        for (const LoadInfo::Tile* t : mips) out.push_back({ (t->tmem - base) * 8, t->line * 64 / bits, t->h, bits, li.dxt == 0 });
        for (auto it = out.begin(); it != out.end();) {
            if (it->at + uint64_t(it->w) * it->h * bits / 8 > li.bytes) it = out.erase(it);
            else ++it;
        }
        return out;
    }

    // The byte of texel (x, y) in its level (its low nibble too, for 4-bit: odd x).
    size_t texel_byte(const Level& l, uint32_t x, uint32_t y) {
        size_t b = (size_t(y) * l.w + x) * l.bits / 8;
        return l.swap && (y & 1) ? b ^ 4 : b;
    }

    uint8_t texel_index(const Bytes& d, size_t block, const Level& l, uint32_t x, uint32_t y) {
        uint8_t v = d[block + l.at + texel_byte(l, x, y)];
        if (l.bits == 8) return v;
        return x % 2 ? v & 15 : v >> 4;
    }

    void set_texel_index(Bytes& d, size_t at, const Level& l, uint32_t x, uint32_t y, uint8_t value) {
        uint8_t& v = d[at + l.at + texel_byte(l, x, y)];
        if (l.bits == 8) { v = value; return; }
        v = x % 2 ? uint8_t((v & 0xF0) | (value & 15)) : uint8_t((v & 0x0F) | (value << 4));
    }

    // A building texture's windows, from its day colors: the glass entries and each window (a 4-connected run of
    // glass texels whose box it mostly fills, 2 x 2 texels at least) as a mask over level 0.
    struct Windows {
        std::vector<bool> glass;               // per TLUT entry
        uint8_t dark = 0;                      // the darkest entry that isn't glass: an unlit window's
        std::vector<int> window_of;            // level 0 texel -> its window, -1 none
        int count = 0;
        std::vector<std::array<uint32_t, 2>> corner;   // per window: its box's top left (x, y) on level 0
        std::vector<double> glass_tone;        // per entry, 0..1: the glass's own shading, for the light's tone
    };

    bool find_windows(const Bytes& d, const LoadInfo& li, const std::vector<Level>& lv, Windows& out) {
        namespace dl = rush2::diorama_look;
        const Level& l = lv[0];
        if (l.w < 8 || l.h < 8 || li.tlut_n < 4) return false;
        std::vector<dl::Rgb> pal(li.tlut_n);
        std::vector<int> used(li.tlut_n, 0);
        for (uint32_t k = 0; k < li.tlut_n; k++) {
            uint16_t e = u16(d, li.tlut + k * 2);
            auto c5 = [](int v) { return (v << 3) | (v >> 2); };
            pal[k] = { c5((e >> 11) & 31), c5((e >> 6) & 31), c5((e >> 1) & 31) };
        }
        std::vector<uint8_t> idx(size_t(l.w) * l.h);
        for (uint32_t y = 0; y < l.h; y++) {
            for (uint32_t x = 0; x < l.w; x++) {
                uint8_t i = texel_index(d, li.texels, l, x, y);
                if (i >= li.tlut_n) return false;
                idx[size_t(y) * l.w + x] = i;
                used[i]++;
            }
        }
        // The texture's median brightness, by texels.
        std::vector<double> lum;
        for (uint8_t i : idx) lum.push_back(dl::luminance(pal[i]));
        std::nth_element(lum.begin(), lum.begin() + lum.size() / 2, lum.end());
        const double median = lum[lum.size() / 2];
        out.glass.assign(li.tlut_n, false);
        size_t glass_texels = 0;
        for (uint32_t k = 0; k < li.tlut_n; k++) {
            if (!used[k]) continue;
            const dl::Rgb& c = pal[k];
            const double lk = dl::luminance(c);
            const bool green = c[1] > c[0] + 12 && c[1] > c[2] + 6;
            const bool blue = c[2] > c[0] + 30 && c[2] > c[1] + 12;
            if (green || (u16(d, li.tlut + k * 2) & 1) == 0) continue;     // foliage, see-through
            if (lk < median * 0.55 || (blue && lk < median * 1.1)) out.glass[k] = true;
        }
        for (uint8_t i : idx) glass_texels += out.glass[i];
        const double share = double(glass_texels) / idx.size();
        if (share < 0.05 || share > 0.6) return false;
        // A sign's colors (a chevron board's yellow) are far more saturated than a building's walls (brick at most).
        double wall_sat = 0;
        for (uint8_t i : idx) {
            if (out.glass[i]) continue;
            const dl::Rgb& c = pal[i];
            wall_sat += std::max({ c[0], c[1], c[2] }) - std::min({ c[0], c[1], c[2] });
        }
        if (wall_sat / double(idx.size() - glass_texels) > 110) return false;
        // The unlit windows' entry: the darkest used entry that isn't glass (else the darkest glass entry, which then
        // stays unlit).
        double darkest = 1e9;
        int dark = -1;
        for (uint32_t k = 0; k < li.tlut_n; k++) {
            if (used[k] && !out.glass[k] && (u16(d, li.tlut + k * 2) & 1) && dl::luminance(pal[k]) < darkest) {
                darkest = dl::luminance(pal[k]);
                dark = int(k);
            }
        }
        if (dark < 0 || darkest > median) {
            // No dark wall entry: the darkest glass entry is kept as the unlit glass.
            darkest = 1e9;
            for (uint32_t k = 0; k < li.tlut_n; k++) {
                if (out.glass[k] && dl::luminance(pal[k]) < darkest) { darkest = dl::luminance(pal[k]); dark = int(k); }
            }
            out.glass[size_t(dark)] = false;
        }
        out.dark = uint8_t(dark);
        // Windows: 4-connected runs of glass texels (not wrapping), kept when 6+ texels, 2 x 2 at least, filling three
        // quarters of their box or more (panes are rectangles; a sign's chevrons and a rail's gaps aren't).
        out.window_of.assign(idx.size(), -1);
        std::vector<int> comp(idx.size(), -1);
        int next = 0;
        for (size_t s = 0; s < idx.size(); s++) {
            if (comp[s] >= 0 || !out.glass[idx[s]]) continue;
            std::vector<size_t> stack{ s }, members;
            comp[s] = next;
            uint32_t x0 = l.w, x1 = 0, y0 = l.h, y1 = 0;
            while (!stack.empty()) {
                size_t p = stack.back();
                stack.pop_back();
                members.push_back(p);
                uint32_t x = uint32_t(p % l.w), y = uint32_t(p / l.w);
                x0 = std::min(x0, x), x1 = std::max(x1, x), y0 = std::min(y0, y), y1 = std::max(y1, y);
                const int64_t nb[4][2] = { { 1, 0 }, { -1, 0 }, { 0, 1 }, { 0, -1 } };
                for (const auto& n : nb) {
                    int64_t nx = int64_t(x) + n[0], ny = int64_t(y) + n[1];
                    if (nx < 0 || ny < 0 || nx >= l.w || ny >= l.h) continue;
                    size_t q = size_t(ny) * l.w + size_t(nx);
                    if (comp[q] < 0 && out.glass[idx[q]]) { comp[q] = next; stack.push_back(q); }
                }
            }
            next++;
            const uint32_t bw = x1 - x0 + 1, bh = y1 - y0 + 1;
            if (members.size() >= 6 && bw >= 2 && bh >= 2 && members.size() * 5 >= size_t(bw) * bh * 3 &&
                (bw < l.w || bh < l.h)) {
                for (size_t p : members) out.window_of[p] = out.count;
                out.corner.push_back({ x0, y0 });
                out.count++;
            }
        }
        if (out.count == 0) return false;
        size_t window_texels = 0;
        for (int w : out.window_of) window_texels += w >= 0;
        if (window_texels * 20 < idx.size()) return false;
        // The glass's tone: its brightness among the glass entries.
        double lo = 1e9, hi = -1e9;
        for (uint32_t k = 0; k < li.tlut_n; k++) if (out.glass[k]) lo = std::min(lo, dl::luminance(pal[k])), hi = std::max(hi, dl::luminance(pal[k]));
        out.glass_tone.assign(li.tlut_n, 0.5);
        for (uint32_t k = 0; k < li.tlut_n; k++) {
            if (out.glass[k]) out.glass_tone[k] = hi > lo ? (dl::luminance(pal[k]) - lo) / (hi - lo) : 0.5;
        }
        return true;
    }

    // ---- the PlayStation's night art ---------------------------------------------------------------------------------
    // (docs/sfrush_ps1.md#n64-night-art) The PS1 remade three N64 courses with a hand painted night set, and the
    // courses it lacks are built from largely the same N64 textures. tools/sfrush_ps1/n64_night_map.py maps each N64
    // building texture onto the PS1 page texels of the same art (assets/sfrush_ps1/n64_night_map.json: a page, a CLUT
    // and an affine), the PS1 install samples the night set through it from the player's disc (n64_night_art.bin), and
    // here each texture it has is redrawn in those texels: its own TLUT and texels (a copy; the PS1's colors fit the
    // texture's 16 or 256 entries), the load list pointed at them. Texels the PS1's art leaves see-through keep the
    // texture's own night colors.

    std::mutex night_art_mutex;
    std::shared_ptr<const std::vector<uint8_t>> night_art_bytes, snow_art_bytes;
    uint64_t night_art_key = 0, snow_art_key = 0;
    uint64_t art_hash(const std::shared_ptr<const std::vector<uint8_t>>& art) {
        if (!art || art->empty()) return 0;
        uint64_t key = 0xcbf29ce484222325ull;
        for (uint8_t v : *art) key = (key ^ v) * 0x100000001b3ull;
        return key;
    }

    // n64_night_art.bin: "N64NIGHT", u32 count, then per texture u64 key, u16 width, u16 height and width x height
    // u16 RGBA5551 texels (little endian; alpha 0: no PS1 texel there).
    struct ArtTexture { uint32_t w = 0, h = 0; const uint8_t* texels = nullptr; };
    std::map<uint64_t, ArtTexture> read_night_art(const std::vector<uint8_t>& d, size_t* end = nullptr) {
        std::map<uint64_t, ArtTexture> out;
        if (end) *end = d.size();
        if (d.size() < 12 || std::memcmp(d.data(), "N64NIGHT", 8) != 0) return out;
        uint32_t n = uint32_t(d[8]) | uint32_t(d[9]) << 8 | uint32_t(d[10]) << 16 | uint32_t(d[11]) << 24;
        size_t at = 12;
        for (uint32_t i = 0; i < n && at + 12 <= d.size(); i++) {
            uint64_t key = 0;
            for (int k = 7; k >= 0; k--) key = key << 8 | d[at + size_t(k)];
            ArtTexture t{ uint32_t(d[at + 8] | d[at + 9] << 8), uint32_t(d[at + 10] | d[at + 11] << 8), &d[at + 12] };
            at += 12 + size_t(t.w) * t.h * 2;
            if (at > d.size()) break;
            out[key] = t;
        }
        if (end) *end = std::min(at, d.size());
        return out;
    }

    // A texture's key: FNV-1a 64 of u16 width, u16 height (big endian) and its texels' TLUT entries (RGBA5551, big
    // endian) row by row, as n64_night_map.py tex_key computes it from the asset dump's images.
    uint64_t night_key(const Bytes& d, const LoadInfo& li, const Level& l, uint32_t w, uint32_t h) {
        uint64_t x = 0xcbf29ce484222325ull;
        auto add = [&](uint8_t v) { x = (x ^ v) * 0x100000001b3ull; };
        add(uint8_t(w >> 8)), add(uint8_t(w)), add(uint8_t(h >> 8)), add(uint8_t(h));
        for (uint32_t y = 0; y < h; y++) {
            for (uint32_t xx = 0; xx < w; xx++) {
                uint8_t i = texel_index(d, li.texels, l, xx, y);
                uint16_t e = i < li.tlut_n ? u16(d, li.tlut + size_t(i) * 2) : 0;
                add(uint8_t(e >> 8)), add(uint8_t(e));
            }
        }
        return x;
    }

    // Colors (RGBA5551) to at most n entries: the closest two (by RGB, weighted by their texel counts) merged until
    // they fit. A see-through color is kept apart.
    std::vector<uint16_t> fit_palette(const std::map<uint16_t, size_t>& counts, uint32_t n, std::map<uint16_t, uint16_t>& to) {
        struct C { double r, g, b, w; bool clear; std::vector<uint16_t> members; };
        std::vector<C> cs;
        for (const auto& [c, k] : counts) {
            cs.push_back({ double((c >> 11) & 31), double((c >> 6) & 31), double((c >> 1) & 31), double(k), (c & 1) == 0, { c } });
        }
        while (cs.size() > n) {
            double best = 1e18;
            size_t a = 0, bb = 1;
            for (size_t i = 0; i < cs.size(); i++) {
                for (size_t j = i + 1; j < cs.size(); j++) {
                    if (cs[i].clear != cs[j].clear) continue;
                    double dr = cs[i].r - cs[j].r, dg = cs[i].g - cs[j].g, db = cs[i].b - cs[j].b;
                    double cost = (dr * dr + dg * dg + db * db) * cs[i].w * cs[j].w / (cs[i].w + cs[j].w);
                    if (cost < best) best = cost, a = i, bb = j;
                }
            }
            C& x = cs[a];
            const C& y = cs[bb];
            double w = x.w + y.w;
            x.r = (x.r * x.w + y.r * y.w) / w, x.g = (x.g * x.w + y.g * y.w) / w, x.b = (x.b * x.w + y.b * y.w) / w, x.w = w;
            x.members.insert(x.members.end(), y.members.begin(), y.members.end());
            cs.erase(cs.begin() + std::ptrdiff_t(bb));
        }
        std::vector<uint16_t> pal;
        for (const C& c : cs) {
            // A see-through entry keeps its color (apply_night_art's see-through color: black at night).
            uint16_t e = uint16_t(int(std::lround(c.r)) << 11 | int(std::lround(c.g)) << 6 | int(std::lround(c.b)) << 1 | (c.clear ? 0 : 1));
            for (uint16_t m : c.members) to[m] = uint16_t(pal.size());
            pal.push_back(e);
        }
        return pal;
    }

    // Each texture-load list whose texture the art has gets its own texels and TLUT in the PS1's night colors.
    // Returns the lists redrawn. Run before the TLUTs are recolored (the keys are the day colors).
    // view: level 0 as tall as the drawn size the art is of needs (it may run past the list's own tile).
    struct ArtJob { int list; LoadInfo li; std::vector<Level> lv; Level view; uint32_t w, h; ArtTexture art; };
    // Each texture-load list (not the banners') as its texture is drawn: its loads and levels, and each size it is
    // drawn at with that view's key. The sizes: its own tile 0's, the tile 0 size a model sets after calling it (a
    // model often draws part of a list's block: 32 x 64 of a 64 x 64 load), and the block's whole height (a model may
    // draw more rows than the list's tile has, 64 of a 48-row tile, reading on into what follows level 0). The asset
    // dump's images, and so the map's keys, are the texture at each of these.
    struct ListView {
        int list = -1;
        LoadInfo li;
        std::vector<Level> lv;
        struct View { uint32_t w, h; Level level; uint64_t key; };   // level: level 0 as tall as the view needs
        std::vector<View> views;
    };
    std::vector<ListView> list_views(Builder& b, const std::set<int>& banners) {
        std::map<int, std::set<std::pair<uint32_t, uint32_t>>> sizes;
        for (const Builder::ModelOut& m : b.models) {
            for (const auto& lod : m.lods) {
                if (!lod) continue;
                std::map<size_t, const Fixup*> at;
                for (const Fixup& f : lod->fixups) at[f.pos] = &f;
                int current = -1;
                for (size_t o = 0; o + 8 <= lod->bytes.size(); o += 8) {
                    uint32_t w0 = u32(lod->bytes, o), w1 = u32(lod->bytes, o + 4);
                    uint8_t op = uint8_t(w0 >> 24);
                    auto f = at.find(o + 4);
                    if (op == 0xDE) current = f != at.end() && f->second->ref.load ? f->second->ref.index : -1;
                    else if (op == 0xF2 && current >= 0 && ((w1 >> 24) & 7) == 0) {
                        uint32_t w = ((w1 >> 12) & 0xFFF) / 4 - ((w0 >> 12) & 0xFFF) / 4 + 1;
                        uint32_t h = (w1 & 0xFFF) / 4 - (w0 & 0xFFF) / 4 + 1;
                        sizes[current].insert({ w, h });
                    }
                }
            }
        }
        std::vector<ListView> out;
        for (size_t i = 0; i < b.load_lists.size(); i++) {
            if (banners.count(int(i))) continue;
            ListView v;
            v.list = int(i);
            v.li = read_load(b, b.load_lists[i]);
            const LoadInfo& li = v.li;
            if (li.tlut == SIZE_MAX || li.texels == SIZE_MAX || li.tlut_fix == SIZE_MAX || li.texel_fix == SIZE_MAX ||
                li.texels + li.bytes > b.out.size() || li.tlut + li.tlut_n * 2 > b.out.size()) continue;
            v.lv = levels(li);
            if (v.lv.empty()) continue;
            const Level& l0 = v.lv[0];
            const LoadInfo::Tile& t0 = li.tiles[0];
            const uint32_t rows = l0.w ? uint32_t((uint64_t(li.bytes - l0.at) * 8 / l0.bits) / l0.w) : 0;
            std::set<std::pair<uint32_t, uint32_t>> drawn = sizes[int(i)];
            for (uint32_t w : { t0.w && t0.w <= l0.w ? t0.w : l0.w, l0.w }) drawn.insert({ w, l0.h }), drawn.insert({ w, rows });
            for (const auto& [w, h] : drawn) {
                if (w == 0 || h == 0 || w > l0.w || h > rows) continue;
                Level view = l0;
                view.h = std::max(l0.h, h);
                v.views.push_back({ w, h, view, night_key(b.out, li, view, w, h) });
            }
            out.push_back(std::move(v));
        }
        return out;
    }

    // The lists whose texture the art has: the largest drawn size it has (it holds the smaller ones' texels too).
    std::vector<ArtJob> find_night_art(Builder& b, const std::map<uint64_t, ArtTexture>& art, const std::set<int>& banners) {
        std::vector<ArtJob> jobs;
        for (const ListView& v : list_views(b, banners)) {
            std::optional<ArtJob> pick;
            for (const ListView::View& w : v.views) {
                auto it = art.find(w.key);
                if (it == art.end() || it->second.w != w.w || it->second.h != w.h) continue;
                if (!pick || w.w * w.h > pick->w * pick->h) pick = ArtJob{ v.list, v.li, v.lv, w.level, w.w, w.h, it->second };
            }
            if (pick) jobs.push_back(*pick);
        }
        return jobs;
    }

    // Run after the TLUTs are recolored: a texel the PS1's art has no texel for keeps its own (night) color, as do
    // the block's texels outside the drawn size the art is of. The texture's see-through texels stay so, all in one
    // entry: black at night; with `mean_see_through` (snow) the mean of the art's colors, since most track textures
    // are drawn without an alpha test and show those texels (black specks on The Rock's snowy banks).
    void apply_night_art(Builder& b, const ArtJob& j, bool mean_see_through = false) {
        const Level& l0 = j.lv[0];
        const Level& v0 = j.view;   // level 0, or taller when the art's drawn size runs past it
        // Colors over the view: the PS1's texel, or (none there) the texture's own; its see-through texels stay so.
        std::vector<uint16_t> color(size_t(v0.w) * v0.h);
        std::map<uint16_t, size_t> counts;
        uint16_t see_through = 0;
        if (mean_see_through) {
            double sum[3] = { 0, 0, 0 };
            size_t n = 0;
            for (size_t k = 0; k < size_t(j.w) * j.h; k++) {
                uint16_t ps1 = uint16_t(j.art.texels[k * 2] | j.art.texels[k * 2 + 1] << 8);
                if (!(ps1 & 1)) continue;
                sum[0] += (ps1 >> 11) & 31; sum[1] += (ps1 >> 6) & 31; sum[2] += (ps1 >> 1) & 31;
                n++;
            }
            if (n) see_through = uint16_t(std::lround(sum[0] / n) << 11 | std::lround(sum[1] / n) << 6 | std::lround(sum[2] / n) << 1);
        }
        for (uint32_t y = 0; y < v0.h; y++) {
            for (uint32_t x = 0; x < v0.w; x++) {
                uint8_t i = texel_index(b.out, j.li.texels, v0, x, y);
                uint16_t own = i < j.li.tlut_n ? u16(b.out, j.li.tlut + size_t(i) * 2) : 0;
                uint16_t c = own;
                if (x < j.w && y < j.h && (own & 1)) {
                    const uint8_t* a = j.art.texels + (size_t(y) * j.w + x) * 2;
                    uint16_t ps1 = uint16_t(a[0] | a[1] << 8);
                    if (ps1 & 1) c = ps1;
                }
                if ((own & 1) == 0) c = see_through;
                color[size_t(y) * v0.w + x] = c;
                counts[c]++;
            }
        }
        std::map<uint16_t, uint16_t> to;
        std::vector<uint16_t> pal = fit_palette(counts, j.li.tlut_n, to);
        Bytes texels(b.out.begin() + j.li.texels, b.out.begin() + j.li.texels + j.li.bytes);
        // The mipmaps from the level-0 texel under their texels' centers, then the view itself (rows past level 0 are
        // where the mipmaps are stored: the view as drawn wins there).
        for (size_t k = 1; k < j.lv.size(); k++) {
            const Level& level = j.lv[k];
            for (uint32_t y = 0; y < level.h; y++) {
                for (uint32_t x = 0; x < level.w; x++) {
                    uint32_t x0 = std::min(l0.w - 1, uint32_t((x + 0.5) * l0.w / level.w));
                    uint32_t y0 = std::min(l0.h - 1, uint32_t((y + 0.5) * l0.h / level.h));
                    set_texel_index(texels, 0, level, x, y, uint8_t(to[color[size_t(y0) * v0.w + x0]]));
                }
            }
        }
        for (uint32_t y = 0; y < v0.h; y++) {
            for (uint32_t x = 0; x < v0.w; x++) set_texel_index(texels, 0, v0, x, y, uint8_t(to[color[size_t(y) * v0.w + x]]));
        }
        Bytes tlut(size_t(j.li.tlut_n) * 2, 0);
        for (size_t k = 0; k < pal.size(); k++) put16(tlut, k * 2, pal[k]);
        Bytes raw = texels;
        const uint32_t tlut_at = uint32_t((raw.size() + 7) & ~size_t(7));
        raw.resize(tlut_at, 0);
        raw.insert(raw.end(), tlut.begin(), tlut.end());
        int s = b.add_raw(raw);
        for (Fixup& f : b.load_lists[size_t(j.list)].fixups) {
            if (f.pos == j.li.texel_fix) f.ref = { false, s, 0 };
            else if (f.pos == j.li.tlut_fix) f.ref = { false, s, tlut_at };
        }
    }

    // Per texture-load list, the triangles the models draw after a call to it: (all, walls, walls 12 ft high or more).
    std::vector<std::array<size_t, 3>> list_faces(const Builder& b) {
        auto address = [&](const Ref& r) { return size_t(b.sources[size_t(r.index)].second) + r.offset; };
        std::vector<std::array<size_t, 3>> faces(b.load_lists.size(), { 0, 0, 0 });
        for (const Builder::ModelOut& m : b.models) {
            for (const auto& lod : m.lods) {
                if (!lod) continue;
                std::map<size_t, const Fixup*> at;
                for (const Fixup& f : lod->fixups) at[f.pos] = &f;
                std::array<std::array<double, 3>, 64> slot{};
                std::array<bool, 64> valid{};
                int current = -1;
                for (size_t o = 0; o + 8 <= lod->bytes.size(); o += 8) {
                    uint32_t w0 = u32(lod->bytes, o), w1 = u32(lod->bytes, o + 4);
                    uint8_t op = uint8_t(w0 >> 24);
                    auto f = at.find(o + 4);
                    if (op == 0xDE && f != at.end() && f->second->ref.load) current = f->second->ref.index;
                    else if (op == 0x01 && f != at.end() && !f->second->ref.load) {
                        uint32_t n = (w0 >> 12) & 0xFF, end = (w0 >> 1) & 0x7F;
                        size_t from = address(f->second->ref);
                        for (uint32_t k = 0; k < n && end >= n && end - n + k < 64; k++) {
                            if (from + k * 16 + 6 > b.out.size()) break;
                            slot[end - n + k] = { double(s16(b.out, from + k * 16)), double(s16(b.out, from + k * 16 + 2)),
                                                  double(s16(b.out, from + k * 16 + 4)) };
                            valid[end - n + k] = true;
                        }
                    }
                    else if ((op == 0x05 || op == 0x06) && current >= 0) {
                        auto tri = [&](uint32_t w) {
                            uint32_t i0 = ((w >> 16) & 0xFF) / 2 % 64, i1 = ((w >> 8) & 0xFF) / 2 % 64, i2 = (w & 0xFF) / 2 % 64;
                            if (!valid[i0] || !valid[i1] || !valid[i2]) return;
                            const auto &a = slot[i0], &c1 = slot[i1], &c2 = slot[i2];
                            double e1[3] = { c1[0] - a[0], c1[1] - a[1], c1[2] - a[2] }, e2[3] = { c2[0] - a[0], c2[1] - a[1], c2[2] - a[2] };
                            double n[3] = { e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0] };
                            double len = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
                            if (len < 1e-9) return;
                            faces[size_t(current)][0]++;
                            if (std::abs(n[1] / len) < 0.3) {
                                faces[size_t(current)][1]++;
                                double h = std::max({ a[1], c1[1], c2[1] }) - std::min({ a[1], c1[1], c2[1] });
                                if (h >= 12.0 * 16.0) faces[size_t(current)][2]++;
                            }
                        };
                        tri(w0 & 0xFFFFFF);
                        if (op == 0x06) tri(w1 & 0xFFFFFF);
                    }
                }
            }
        }
        return faces;
    }

    void night_track(Builder& b, NightStats& stats) {
        namespace dl = rush2::diorama_look;
        auto address = [&](const Ref& r) { return size_t(b.sources[size_t(r.index)].second) + r.offset; };

        // Which texture-load lists are drawn mostly on walls (70% of their triangles), a third of those tall ones (a
        // building's, not a barrier's or a sign's: wall triangles 12 ft high or more).
        const std::vector<std::array<size_t, 3>> faces = list_faces(b);
        // The banners (CHKPNT, FINISH) keep their lists.
        std::set<int> banners;
        for (const Builder::TextureOut& t : b.textures) if (t.ref.load) banners.insert(t.ref.index);
        // The PS1's night art, when the PS1 mod gave it: its textures are matched now (by their day colors) and redrawn
        // after the recolor below, and no window is lit by guess (the guess lit trim and wall as often as glass).
        const std::shared_ptr<const std::vector<uint8_t>> art_bytes = rush2::track1::night_art();
        const bool have_art = art_bytes && !art_bytes->empty();
        std::vector<ArtJob> art_jobs;
        if (have_art) art_jobs = find_night_art(b, read_night_art(*art_bytes), banners);
        struct Lit { LoadInfo li; std::vector<Level> lv; Windows w; };
        std::map<int, Lit> lit;
        for (size_t i = 0; i < b.load_lists.size() && !have_art; i++) {
            if (banners.count(int(i)) || faces[i][0] == 0 || faces[i][1] * 10 < faces[i][0] * 7 || faces[i][2] * 10 < faces[i][1] * 3) continue;
            Lit l{ read_load(b, b.load_lists[i]) };
            if (l.li.tlut == SIZE_MAX || l.li.texels == SIZE_MAX || l.li.tlut_fix == SIZE_MAX || l.li.texel_fix == SIZE_MAX ||
                l.li.texels + l.li.bytes > b.out.size() || l.li.tlut + l.li.tlut_n * 2 > b.out.size()) continue;
            l.lv = levels(l.li);
            if (l.lv.empty() || !find_windows(b.out, l.li, l.lv, l.w)) continue;
            lit.emplace(int(i), std::move(l));
        }
        std::set<size_t> tluts;                     // RGBA16 TLUT data recolored (container offsets)
        std::map<size_t, uint32_t> tlut_count;
        std::set<size_t> shade_lit, shade_tex;      // vertices (container offsets) drawn untextured / textured
        auto walk = [&](const Body& body, bool model) {
            std::map<size_t, const Ref*> refs;
            for (const Fixup& f : body.fixups) {
                if (!f.ref.load && f.ref.model.empty()) refs[f.pos] = &f.ref;
            }
            size_t timg = SIZE_MAX;
            bool texture_on = true, combine_tex = true;
            std::array<size_t, 64> slot;
            slot.fill(SIZE_MAX);
            for (size_t o = 0; o + 8 <= body.bytes.size(); o += 8) {
                uint32_t w0 = u32(body.bytes, o), w1 = u32(body.bytes, o + 4);
                uint8_t op = uint8_t(w0 >> 24);
                if (op == 0xFD) {
                    auto it = refs.find(o + 4);
                    timg = it != refs.end() ? address(*it->second) : SIZE_MAX;
                }
                else if (op == 0xF0 && timg != SIZE_MAX) {
                    uint32_t n = ((w1 >> 14) & 0x3FF) + 1;
                    uint32_t& have = tlut_count[timg];
                    have = std::max(have, n);
                }
                else if (!model) {
                    continue;
                }
                else if (op == 0xD7) {
                    texture_on = (w0 & 2) != 0;
                }
                else if (op == 0xFC) {
                    combine_tex = combine_textured(w0 & 0xFFFFFF, w1);
                }
                else if (op == 0x01) {
                    auto it = refs.find(o + 4);
                    uint32_t n = (w0 >> 12) & 0xFF, end = (w0 >> 1) & 0x7F;
                    if (it == refs.end() || end < n) continue;
                    for (uint32_t k = 0; k < n && end - n + k < slot.size(); k++) slot[end - n + k] = address(*it->second) + k * 16;
                }
                else if (op == 0x05 || op == 0x06) {
                    auto tri = [&](uint32_t w) {
                        for (uint32_t i : { (w >> 16) & 0xFF, (w >> 8) & 0xFF, w & 0xFF }) {
                            size_t v = slot[(i / 2) % 64];
                            if (v == SIZE_MAX) continue;
                            (texture_on && combine_tex ? shade_tex : shade_lit).insert(v);
                        }
                    };
                    tri(w0 & 0xFFFFFF);
                    if (op == 0x06) tri(w1 & 0xFFFFFF);
                }
            }
        };
        // The banners' TLUTs keep their day colors: the CHECKPOINT and FINISH signs stay lit at night, as the PS1's
        // night versions draw theirs (a TLUT a banner shares with anything else is recolored with it).
        for (size_t i = 0; i < b.load_lists.size(); i++) {
            if (!banners.count(int(i))) continue;
            walk(b.load_lists[i], false);
        }
        std::map<size_t, uint32_t> banner_tluts;
        banner_tluts.swap(tlut_count);
        for (size_t i = 0; i < b.load_lists.size(); i++) {
            if (!banners.count(int(i))) walk(b.load_lists[i], false);
        }
        for (const Builder::ModelOut& m : b.models) {
            for (const auto& lod : m.lods) if (lod) walk(*lod, true);
        }
        for (const auto& [at, n] : banner_tluts) {
            if (!tlut_count.count(at)) stats.banner_tluts++;
        }
        for (const auto& [at, n] : tlut_count) {
            if (at + size_t(n) * 2 > b.out.size()) continue;
            stats.tluts++;
            for (uint32_t k = 0; k < n; k++) {
                uint16_t e = u16(b.out, at + k * 2);
                auto c5 = [](int v) { return (v << 3) | (v >> 2); };
                dl::Rgb c = dl::night({ c5((e >> 11) & 31), c5((e >> 6) & 31), c5((e >> 1) & 31) });
                put16(b.out, at + k * 2, uint16_t(((c[0] >> 3) << 11) | ((c[1] >> 3) << 6) | ((c[2] >> 3) << 1) | (e & 1)));
                stats.entries++;
            }
        }
        for (size_t v : shade_lit) {
            if (shade_tex.count(v)) {
                stats.shared++;
                continue;
            }
            if (v + 16 > b.out.size()) continue;
            dl::Rgb c = dl::night({ b.out[v + 12], b.out[v + 13], b.out[v + 14] });
            for (int k = 0; k < 3; k++) b.out[v + 12 + size_t(k)] = uint8_t(c[size_t(k)]);
            stats.shades++;
        }

        for (const ArtJob& j : art_jobs) apply_night_art(b, j);
        stats.art_textures = art_jobs.size();

        // The lit copies of the building textures (their TLUTs are night colored now).
        constexpr int copies = 2;
        constexpr uint32_t seed = 0x7E1Fu;
        std::map<int, std::array<int, copies>> copy_lists;   // load list -> its copies' load lists
        for (auto& [list, l] : lit) {
            stats.window_textures++;
            stats.windows += size_t(l.w.count);
            std::array<int, copies> made{};
            for (int c = 0; c < copies; c++) {
                // Which windows this copy lights: about half, a floor at a time (the windows whose tops are in the same
                // row), each half of the texture's width on its own, so a glass tower's many small windows light in
                // bands, not as speckle.
                std::vector<bool> on(size_t(l.w.count));
                for (int w = 0; w < l.w.count; w++) {
                    const auto& [cx, cy] = l.w.corner[size_t(w)];
                    int64_t half = cx * 2 / l.lv[0].w;
                    on[size_t(w)] = dl::hash01(int64_t(l.li.texels) + half * 977, int64_t(cy) * 7 + c, seed) < 0.5;
                }
                Bytes texels(b.out.begin() + l.li.texels, b.out.begin() + l.li.texels + l.li.bytes);
                const Level& l0 = l.lv[0];
                for (size_t lv = 0; lv < l.lv.size(); lv++) {
                    const Level& level = l.lv[lv];
                    for (uint32_t y = 0; y < level.h; y++) {
                        for (uint32_t x = 0; x < level.w; x++) {
                            uint8_t i = texel_index(b.out, l.li.texels, level, x, y);
                            if (i >= l.w.glass.size() || !l.w.glass[i]) continue;
                            // The level-0 texel under this one's center.
                            uint32_t x0 = std::min(l0.w - 1, uint32_t((x + 0.5) * l0.w / level.w));
                            uint32_t y0 = std::min(l0.h - 1, uint32_t((y + 0.5) * l0.h / level.h));
                            int w = l.w.window_of[size_t(y0) * l0.w + x0];
                            if (w < 0 || !on[size_t(w)]) set_texel_index(texels, 0, level, x, y, l.w.dark);
                        }
                    }
                }
                Bytes tlut(b.out.begin() + l.li.tlut, b.out.begin() + l.li.tlut + l.li.tlut_n * 2);
                for (uint32_t k = 0; k < l.li.tlut_n; k++) {
                    if (!l.w.glass[k]) continue;
                    dl::Rgb c2 = dl::window_light(0.15 + 0.7 * l.w.glass_tone[k]);
                    put16(tlut, k * 2, uint16_t(((c2[0] >> 3) << 11) | ((c2[1] >> 3) << 6) | ((c2[2] >> 3) << 1) | 1));
                }
                Bytes raw = texels;
                const uint32_t tlut_at = uint32_t((raw.size() + 7) & ~size_t(7));
                raw.resize(tlut_at, 0);
                raw.insert(raw.end(), tlut.begin(), tlut.end());
                int s = b.add_raw(raw);
                Body copy = b.load_lists[size_t(list)];
                for (Fixup& f : copy.fixups) {
                    if (f.pos == l.li.texel_fix) f.ref = { false, s, 0 };
                    else if (f.pos == l.li.tlut_fix) f.ref = { false, s, tlut_at };
                }
                made[size_t(c)] = int(b.load_lists.size());
                b.load_lists.push_back(std::move(copy));
            }
            copy_lists[list] = made;
        }
        // Each call to a building texture: the original (dark), or one of the copies, by the model and the call.
        for (Builder::ModelOut& m : b.models) {
            uint64_t name_hash = 1469598103934665603ull;
            for (char ch : m.name) name_hash = (name_hash ^ uint8_t(ch)) * 1099511628211ull;
            for (auto& lod : m.lods) {
                if (!lod) continue;
                for (Fixup& f : lod->fixups) {
                    if (!f.ref.load) continue;
                    auto it = copy_lists.find(f.ref.index);
                    if (it == copy_lists.end()) continue;
                    stats.calls++;
                    double r = dl::hash01(int64_t(name_hash & 0x7FFFFFFF), int64_t(f.pos), seed + 1);
                    if (r < 0.25) continue;
                    f.ref.index = it->second[r < 0.625 ? 0 : 1];
                    stats.lit_calls++;
                }
            }
        }
    }

    // ---- snow ---------------------------------------------------------------------------------------------------------
    // (docs/sfrush_ps1.md#n64-snow-versions) An N64 course raced as a SNOW version, in the PlayStation SF Rush's snow:
    // the PS1 keeps its day fog and sky and paints snow into its textures (grass, roofs, ledges and trees white, the
    // road kept). The textures n64_night_map.json and n64_snow_map.json map onto the PS1's art wear its snow texels
    // (the mod's n64_snow_art.bin, applied the night art's way); every other texture keeps its day colors (user
    // 2026-11-14: no made-up snow in the first version, so what the PS1 art doesn't cover shows).
    struct SnowStats { size_t art_textures = 0; };

    void snow_track(Builder& b, SnowStats& stats) {
        std::set<int> banners;
        for (const Builder::TextureOut& t : b.textures) if (t.ref.load) banners.insert(t.ref.index);
        const std::shared_ptr<const std::vector<uint8_t>> bytes = rush2::track1::snow_art();
        if (!bytes || bytes->empty()) return;
        std::vector<ArtJob> jobs = find_night_art(b, read_night_art(*bytes), banners);
        for (const ArtJob& j : jobs) apply_night_art(b, j, true);
        stats.art_textures = jobs.size();
    }

    // The night sky (as the PlayStation mod's NIGHT versions' SKY dome, src/sfrush_ps1/ps1_track.cpp): the PS1's screen
    // gradient, grey 64 from 25.6 degrees up to (0, 0, 25) at the horizon, as a dome model SKYO1 that Rush 2's sky code
    // draws round the camera before the track without depth or fog (the Las Vegas branch of sky_setup_800A45A8,
    // src/rush2049/track2049.cpp rush2_track49_sky). It keeps its world height, so it is centered at `center_y` (the
    // track's sections' median height, world units); its radius is near the s16 limit (1,875 ft): drawn without depth,
    // its size doesn't show. The gradient is a texture (4 x 32 RGBA16, clamped) drawn as TEXEL0 alone: a list of shade
    // alone drew black in game (it leans on the geometry mode the sky code leaves, which a textured sky doesn't).
    void add_night_sky(Builder& b, double center_y) {
        constexpr double top_elevation = 25.6, radius = 30000.0;   // degrees, model units (1/16 ft)
        constexpr double pi = 3.14159265358979323846;
        const double top[3] = { 64, 64, 64 }, horizon[3] = { 0, 0, 25 };
        const double rings[] = { -20, -5, 0, 4, 8, 12, 16, 20, 25.6, 40, 60, 80, 90 };
        constexpr int segments = 32;
        const double cy = std::clamp(center_y * 16.0, -2000.0, 2000.0);
        constexpr int rows = 32;
        // The gradient: row 0 the top's grey, the last row the horizon's blue.
        Bytes texels;
        for (int r = 0; r < rows; r++) {
            double k = 1.0 - double(r) / (rows - 1);
            int c[3];
            for (int i = 0; i < 3; i++) c[i] = int(std::lround(horizon[i] + (top[i] - horizon[i]) * k));
            for (int x = 0; x < 4; x++) add16(texels, uint16_t(((c[0] >> 3) << 11) | ((c[1] >> 3) << 6) | ((c[2] >> 3) << 1) | 1));
        }
        const int tex_raw = b.add_raw(texels);
        Bytes verts;
        auto vertex = [&](double elevation, double azimuth) {
            double e = elevation * pi / 180.0, a = azimuth * pi / 180.0;
            double k = std::clamp(elevation / top_elevation, 0.0, 1.0);
            add16(verts, uint16_t(int16_t(std::lround(radius * std::cos(e) * std::cos(a)))));
            add16(verts, uint16_t(int16_t(std::lround(std::clamp(cy + radius * std::sin(e), -32768.0, 32767.0)))));
            add16(verts, uint16_t(int16_t(std::lround(radius * std::cos(e) * std::sin(a)))));
            add16(verts, 0);
            // s, t (S10.5 texels): the middle of the texture across, its row by elevation.
            add16(verts, uint16_t(2 * 32));
            add16(verts, uint16_t(std::lround(((1.0 - k) * (rows - 1) + 0.5) * 32.0)));
            for (int c = 0; c < 4; c++) verts.push_back(255);
        };
        Body body;
        // 1-cycle TEXEL0 (G_CC_DECALRGBA), opaque, depth and fog off, as the custom tracks' SKY lists
        // (track_mod_convert.cpp), the gradient loaded (RGBA16, TLUT off, clamped); then the track's state put back
        // (depth and fog on, 2-cycle, the RGBA16 TLUT).
        const uint32_t head[] = { 0xE3001001, 0, 0xE3000F00, 0, 0xE3000A01, 0, 0xFCFFFFFF, 0xFFFCF279,
                                  0xE200001C, 0x0F0A4000, 0xD9FEFFFE, 0, 0xD7000002, 0xFFFFFFFF };
        for (uint32_t w : head) add32(body.bytes, w);
        body.fixups.push_back({ body.bytes.size() + 4, { false, tex_raw, 0 } });
        const uint32_t load[] = { 0xFD100000, 0,                                   // SETTIMG RGBA16
                                  0xF5100000, 0x07000000,                          // SETTILE 7 (load)
                                  0xE6000000, 0,                                   // LOADSYNC
                                  0xF3000000, 0x07000000 | ((4 * rows - 1) << 12) | 0x800,   // LOADBLOCK, 1 word a row
                                  0xE7000000, 0,                                   // PIPESYNC
                                  0xF5100200, 0x00080200,                          // SETTILE 0: line 1, clamped
                                  0xF2000000, 0x0000C000 | ((rows - 1) << 2) };    // SETTILESIZE 4 x rows
        for (uint32_t w : load) add32(body.bytes, w);
        int raw = -1;
        // Each band of a ring pair: 2 x 32 vertices, loaded 8 quads (32 vertices) at a time.
        std::vector<std::array<uint32_t, 2>> loads;   // (first vertex, count)
        for (size_t r = 0; r + 1 < std::size(rings); r++) {
            for (int s0 = 0; s0 < segments; s0 += 8) {
                uint32_t first = uint32_t(verts.size() / 16);
                for (int s = s0; s < s0 + 8; s++) {
                    double a0 = 360.0 * s / segments, a1 = 360.0 * (s + 1) / segments;
                    vertex(rings[r], a0), vertex(rings[r], a1), vertex(rings[r + 1], a0), vertex(rings[r + 1], a1);
                }
                loads.push_back({ first, 32 });
            }
        }
        raw = b.add_raw(verts);
        for (const auto& [first, n] : loads) {
            body.fixups.push_back({ body.bytes.size() + 4, { false, raw, first * 16 } });
            add32(body.bytes, 0x01000000 | (n << 12) | (n << 1));
            add32(body.bytes, 0);
            for (uint32_t q = 0; q < n; q += 4) {
                // Faces toward the dome's middle (seen from inside): (a0 low, a0 high, a1 low) and (a1 low, a0 high,
                // a1 high) wind that way with x = cos, z = sin.
                uint32_t v0 = q, v1 = q + 1, v2 = q + 2, v3 = q + 3;
                add32(body.bytes, 0x06000000 | (v0 * 2) << 16 | (v2 * 2) << 8 | (v1 * 2));
                add32(body.bytes, (v1 * 2) << 16 | (v2 * 2) << 8 | (v3 * 2));
            }
        }
        const uint32_t tail[] = { 0xD9FFFFFF, 0x00010001, 0xE3000A01, 0x00100000, 0xE3001001, 0x00008000, 0xFC26A004, 0x1FFC93F8,
                                  0xD7000002, 0xFFFFFFFF, 0xDF000000, 0 };
        for (uint32_t w : tail) add32(body.bytes, w);
        Builder::ModelOut m{ "SKYO1", 1, {}, {}, {}, float(radius / 16.0) };
        m.lods[0] = std::move(body);
        b.models.push_back(std::move(m));
    }

    Bytes convert_geometry(const std::vector<uint8_t>& rom, const Main& main, int t, const std::string& prefix,
                           std::map<std::string, std::string>& names, std::map<std::string, int>& base_shifts,
                           rush2::diorama_look::Look look) {
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
        for (const char* n : r1_shared_objects) {
            if (names.count(n)) continue;
            for (uint32_t i = 0; i < objects.n_models; i++) {
                if (objects.name(i) == n) {
                    names[n] = safe_name(n);
                    b.add_model(so, i, names[n]);
                }
            }
            if (!names.count(n)) fail(std::string("object model ") + n + " missing");
        }
        // Placed breakables get the behaviour id of the Rush 2 model they stand in for, and their origin at their base
        // like Rush 2's breakable models (Rush 1's are centred): Rush 2 knocks an object over about its origin, and its
        // car hit test (func_8008B0CC) takes the placement point within about 3 world units of the car's height, so a
        // centred traffic light, tree or trash muncher would never be hit. The records move down to match, and so do
        // the models of the same family (the name without its number: FLAG2L0-9 animation frames, WINDOWBL1-7 pieces).
        for (auto& m : b.models) {
            if (m.name == names.at(r1_key_model)) m.kind = key_behaviour;
        }
        base_shifts.clear();
        auto family_of = [](const std::string& n) { return n.substr(0, n.find_last_not_of("0123456789") + 1); };
        std::map<std::string, std::set<std::string>> placed_families;
        for (const std::string& n : placed_objects(rom, t)) {
            const ObjectClass* c = object_class(n);
            auto it = names.find(n);
            if (c == nullptr || it == names.end()) continue;
            for (auto& m : b.models) {
                if (m.name == it->second) m.kind = c->kind;
            }
            placed_families[family_of(n)].insert(n);
        }
        for (const auto& [f, placed] : placed_families) {
            std::set<std::string> placed_models, family;
            for (const std::string& n : placed) placed_models.insert(names.at(n));
            for (const auto& [n, renamed] : names) {
                if (family_of(n) == f) family.insert(renamed);
            }
            int shift = b.base_origin(placed_models, family);
            for (const std::string& n : placed) base_shifts[n] = shift;
        }
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
        if (look == rush2::diorama_look::Look::snow) {
            SnowStats stats;
            snow_track(b, stats);
            printf("[Rush1] track %d snow: %zu textures in the PS1's snow art\n", t + 1, stats.art_textures);
        }
        if (look == rush2::diorama_look::Look::night) {
            NightStats stats;
            night_track(b, stats);
            // The sky's height: the median of the track's sections' (placement records +0x38, world units).
            Bytes d = asset(rom, placement_asset + t);
            std::vector<float> ys;
            uint32_t base = u32(d, 4);
            for (size_t o = base; o + 0x64 <= d.size(); o += 0x64) ys.push_back(f32(d, o + 0x38));
            std::sort(ys.begin(), ys.end());
            add_night_sky(b, ys.empty() ? 0.0 : ys[ys.size() / 2]);
            printf("[Rush1] track %d night: %zu TLUTs (%zu entries), %zu untextured shades (%zu shared with textured faces kept), "
                   "%zu building textures with %zu windows, %zu of %zu binds lit, %zu textures in the PS1's night art, %zu banner TLUTs kept lit\n", t + 1,
                   stats.tluts, stats.entries, stats.shades, stats.shared, stats.window_textures, stats.windows, stats.lit_calls,
                   stats.calls, stats.art_textures, stats.banner_tluts);
        }
        return b.build();
    }

    // ---------------------------------------------------------------------------------------------------------------
    // Placement

    Bytes convert_placement(const std::vector<uint8_t>& rom, int t, const std::string& prefix,
                            const std::map<std::string, std::string>& names,
                            const std::map<std::string, int>& base_shifts,
                            std::map<std::string, std::string>& record_models) {
        record_models.clear();
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
        std::set<int> keys;
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
                if (starts_with(n, r1_fence) && s16(k, 0x4A) != 0) n = r1_gate;
                std::optional<std::string> renamed;
                bool world = false;
                for (auto [from, to] : r1_emitters) {
                    if (n == from) { renamed = to; world = true; }
                }
                if (!world && starts_with(n, r1_key)) {
                    int number = s16(k, 0x4A);
                    if (number < 1 || number > r1_keys_max || !keys.insert(number).second) {
                        fail("key " + n + " has number " + std::to_string(number));
                    }
                    renamed = "KEY" + std::to_string(number);
                    record_models[*renamed] = names.at(r1_key_model);
                    world = true;
                }
                bool dropped = false;
                for (const char* p : r1_dropped) dropped = dropped || starts_with(n, p);
                if (!world && !dropped) {
                    auto nt = names.find(n);
                    if (nt == names.end()) fail("object " + n + " has no model");
                    renamed = nt->second;
                    if (const ObjectClass* oc = object_class(n)) {
                        std::string record = oc->record ? oc->record + n.substr(strlen(oc->prefix)) : n;
                        if (record.size() > 15 || !r2_classified(record)) fail("no class record for " + n);
                        record_models[record] = nt->second;
                        renamed = record;
                        world = true;
                    }
                }
                if (!renamed) continue;
                put_name(k, 0, *renamed);
                if (world) {
                    auto shift = base_shifts.find(n);
                    double down = shift == base_shifts.end() ? 0.0 : shift->second / 16.0;
                    for (int a = 0; a < 3; a++) {
                        putf(k, 0x34 + a * 4, double(f32(k, 0x34 + a * 4)) + double(ppos[a]) - (a == 1 ? down : 0.0));
                    }
                }
                put32(k, 0x40, world ? 0x40 : (u32(k, 0x40) & ~0x1000u));
                put32(k, 0x48, 0);
                for (int a = 0; a < 6; a++) putf(k, 0x4C + a * 4, 0.0);
                sec.kids.push_back(std::move(k));
            }
            sections.push_back(std::move(sec));
        }
        // The PS1 SF Rush's keys whose spot this course has (ps1_keys.inc, from tools/arcade_collectibles.py;
        // docs/sfrush_ps1.md#keys): KEYP1-4, made from one of the course's own keys with the PS1 key's record name and
        // world position, each a child of the section whose origin is nearest it (the game culls it with that section).
        struct Ps1Key {
            int course;
            const char* record;
            int bit;
            float pos[3];
        };
        static const Ps1Key ps1_keys[] = {
#include "ps1_keys.inc"
        };
        const Bytes* key_template = nullptr;
        for (const Section& s : sections) {
            for (const Bytes& k : s.kids) {
                std::string n = cname(k, 0);
                if (key_template == nullptr && n.size() == 4 && n.compare(0, 3, "KEY") == 0 && n[3] >= '1' && n[3] <= '8') {
                    key_template = &k;
                }
            }
        }
        std::vector<std::pair<size_t, Bytes>> ps1_kids;
        for (const Ps1Key& pk : ps1_keys) {
            if (pk.course != t || key_template == nullptr || sections.empty()) continue;
            size_t best = 0;
            double best_d = -1.0;
            for (size_t i = 0; i < sections.size(); i++) {
                double dx = f32(sections[i].rec, 0x34) - pk.pos[0], dz = f32(sections[i].rec, 0x3C) - pk.pos[2];
                if (best_d < 0.0 || dx * dx + dz * dz < best_d) { best = i; best_d = dx * dx + dz * dz; }
            }
            Bytes k = *key_template;
            put_name(k, 0, pk.record);
            for (int a = 0; a < 9; a++) putf(k, 0x10 + a * 4, a % 4 == 0 ? 1.0 : 0.0);
            for (int a = 0; a < 3; a++) putf(k, 0x34 + a * 4, double(pk.pos[a]));
            record_models[pk.record] = names.at(r1_key_model);
            ps1_kids.emplace_back(best, std::move(k));
        }
        for (auto& [i, k] : ps1_kids) sections[i].kids.push_back(std::move(k));
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

    // Rush 1's lane speeds are in ft/s (its driver, func_8007CDD4, compares lane x 1.05 with the car's speed); Rush 2's
    // are in mph (x 1.4667 = 22 / 15, func_80074990). Rounded, and a moving point stays moving.
    uint8_t lane_mph(uint8_t fps) {
        return fps == 0 ? 0 : uint8_t(std::max(1, (fps * 30 + 22) / 44));
    }

    struct Checkpoint {
        double pos[3];
        int flags;
    };

    struct Gate {
        double pos[3];
        double dir[3];
        uint32_t r2;
        int flags;
        long k = 0; // the spine point it sits on
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

    int64_t dist2(const LanePoint& p, const LanePoint& q) {
        int64_t dx = p.x - q.x, dy = p.y - q.y, dz = p.z - q.z;
        return dx * dx + dy * dy + dz * dz;
    }

    // Branches for the lanes' own routes. Rush 2 tracks a car on the spine, and only looks at branches when the car is
    // over 40 ft from it (func_80090570); its wrong-way check, respawn point and checkpoint window all come from that.
    // Rush 1 tracks 4 and 5 have alternate routes that only some lanes drive: each stretch of a lane over 40 ft from
    // the spine and the branches so far that gets over 100 ft from them (an alternate route, not a lane on the far side
    // of a wide road) becomes a branch, extended until it is back within 20 ft (at most 30 points each way). Rush 2
    // links branch ends to the nearest spine or branch point at load (func_80092060) and walks branches forward when it
    // moves a respawn point on, so a stretch that would rejoin the spine behind where it left (the lanes closing their
    // lap on track 6) is left out.
    std::vector<std::vector<LanePoint>> path_branches(const std::vector<std::vector<LanePoint>>& lanes) {
        const auto& spine = lanes[0];
        std::vector<std::vector<LanePoint>> out;
        auto nearest_spine = [&](const LanePoint& p) {
            size_t best = 0;
            for (size_t j = 1; j < spine.size(); j++) {
                if (dist2(spine[j], p) < dist2(spine[best], p)) best = j;
            }
            return long(best);
        };
        for (size_t l = 1; l < lanes.size(); l++) {
            const auto& pts = lanes[l];
            std::vector<int64_t> dd(pts.size());
            for (size_t k = 0; k < pts.size(); k++) {
                int64_t best = INT64_MAX;
                for (const LanePoint& q : spine) best = std::min(best, dist2(pts[k], q));
                for (const auto& b : out) {
                    for (const LanePoint& q : b) best = std::min(best, dist2(pts[k], q));
                }
                dd[k] = best;
            }
            size_t k = 0;
            while (k < pts.size()) {
                if (dd[k] <= 1600) {
                    k++;
                    continue;
                }
                size_t a = k;
                while (k < pts.size() && dd[k] > 1600) k++;
                size_t b = k - 1;
                if (*std::max_element(dd.begin() + long(a), dd.begin() + long(b) + 1) <= 10000) continue;
                size_t s = a, e = b;
                while (s > 0 && a - s < 30 && dd[s] > 400) s--;
                while (e < pts.size() - 1 && e - b < 30 && dd[e] > 400) e++;
                long leave = nearest_spine(pts[s]), rejoin = nearest_spine(pts[e]);
                if (0 < rejoin - leave && rejoin - leave < long(spine.size() / 2) && out.size() < 15) {
                    out.emplace_back(pts.begin() + long(s), pts.begin() + long(e) + 1);
                }
            }
        }
        return out;
    }

    // Rush 1 counts a checkpoint once the car's place on the track passes it, however far to the side the car is
    // (func_8009FFCC); Rush 2's gate also needs the car within its radius, and a missed gate keeps the car's checkpoint
    // window (and so its respawn point) behind it. Each gate gets the largest of these radii that keeps every crossing
    // where it was and that no path crosses (within the radius plus a margin) on its way from the previous gate.
    void widen_gates(std::vector<Gate>& gates, const std::vector<std::vector<LanePoint>>& lanes,
                     const std::vector<size_t>& loops, const std::vector<std::array<size_t, 4>>& es) {
        constexpr int radii[] = { 300, 250, 200, 160, 130, 100, 80 };
        constexpr double margin = 30.0;
        if (gates.size() < 2) return;
        auto ok = [&](size_t i) {
            const Gate& g = gates[i];
            const Gate& prev = gates[i == 0 ? gates.size() - 1 : i - 1];
            double lim = std::sqrt(double(g.r2)) + margin;
            lim *= lim;
            // Path 0 is the spine (lane 0), 1-4 the lanes.
            for (size_t path = 0; path < 5; path++) {
                size_t l = path == 0 ? 0 : path - 1;
                const auto& pts = lanes[l];
                int c = crossing(g, pts);
                if (std::abs(c - (path == 0 ? g.k : long(es[i][l]))) > 40) return false;
                size_t j = size_t(crossing(prev, pts));
                bool reached = false;
                for (size_t step = 0; step < pts.size(); step++) {
                    size_t nx = j + 1 < pts.size() ? j + 1 : loops[l];
                    if (long(nx) == c) {
                        reached = true;
                        break;
                    }
                    const LanePoint& p = pts[j];
                    const LanePoint& q = pts[nx];
                    double sp = (p.x - g.pos[0]) * g.dir[0] + (p.z - g.pos[2]) * g.dir[2];
                    double sq = (q.x - g.pos[0]) * g.dir[0] + (q.z - g.pos[2]) * g.dir[2];
                    if ((sp < 0) != (sq < 0)) {
                        double dp = (p.x - g.pos[0]) * (p.x - g.pos[0]) + (p.z - g.pos[2]) * (p.z - g.pos[2]);
                        double dq = (q.x - g.pos[0]) * (q.x - g.pos[0]) + (q.z - g.pos[2]) * (q.z - g.pos[2]);
                        if (std::min(dp, dq) <= lim) return false;
                    }
                    j = nx;
                }
                if (!reached) return false;
            }
            return true;
        };
        std::vector<uint32_t> base;
        for (const Gate& g : gates) base.push_back(g.r2);
        for (size_t i = 0; i < gates.size(); i++) {
            for (int rad : radii) {
                if (uint32_t(rad * rad) <= base[i]) break;
                gates[i].r2 = uint32_t(rad * rad);
                if (ok(i)) break;
                gates[i].r2 = base[i];
            }
        }
        // Widening a gate moves its crossings, which bound the next gate's stretch: put back any gate that no longer
        // holds.
        for (bool changed = true; changed;) {
            changed = false;
            for (size_t i = 0; i < gates.size(); i++) {
                if (gates[i].r2 != base[i] && !ok(i)) {
                    gates[i].r2 = base[i];
                    changed = true;
                }
            }
        }
    }

    Bytes convert_path(const std::vector<uint8_t>& rom, const Main& main, int t, bool backward,
                       std::vector<int16_t>& demo, float& lap_seconds,
                       rush2::track1::ConvertedTrack::Timing& timing) {
        Bytes d = asset(rom, (backward ? path_back_asset : path_asset) + t);
        std::vector<std::vector<LanePoint>> lanes;
        std::vector<size_t> loops;
        std::vector<rush2::track1::FileLane> file_lanes;
        if (!rush2::track1::parse_lanes(d.data(), d.size(), file_lanes)) fail("bad lane file");
        for (const auto& fl : file_lanes) {
            std::vector<LanePoint> pts;
            for (const auto& p : fl.points) pts.push_back({ p.x, p.y, p.z, lane_mph(p.fps), p.behaviour });
            loops.push_back(fl.loop);
            lanes.push_back(std::move(pts));
        }

        // Records: f32 x, y, z, s32 gate radius squared, s16 flags, s16 time[3] (by lap); the end record (flags -1) holds the start time's
        // base at +0x12 and the loop-start checkpoint at +0x14.
        std::vector<Checkpoint> cps;
        int loop_cp = 0;
        timing.checkpoints.clear();
        timing.no_wrong_way = 0;
        timing.radius2.clear();
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
            if (flags & 4) timing.no_wrong_way |= uint16_t(1u << timing.checkpoints.size());
            timing.radius2.push_back(main.w(r + 12));
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
        std::vector<std::vector<LanePoint>> branches = path_branches(lanes);
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
                if (ok) {
                    found = g;
                    found->k = kk;
                }
            }
            if (!found) fail("no gate for checkpoint " + std::to_string(i));
            gates.push_back(*found);
        }
        widen_gates(gates, lanes, loops, es);

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
        route[8] = uint8_t(branches.size());
        out.insert(out.end(), route.begin(), route.end());
        size_t total = n;
        for (const auto& b : branches) {
            // alt 0; from, to and checkpoint -1 (linked at load); point count
            Bytes rec{ 0, 0xFF, 0, 0, 0xFF, 0, 0, 0, 0xFF, 0, 0, 0, 0, 0, 0, 0 };
            put16(rec, 0xA, uint16_t(b.size()));
            out.insert(out.end(), rec.begin(), rec.end());
            total += b.size();
        }
        add16(out, uint16_t(total));
        for (const LanePoint& p : spine) {
            add16(out, uint16_t(int16_t(p.x))); add16(out, uint16_t(int16_t(p.y))); add16(out, uint16_t(int16_t(p.z)));
        }
        for (const auto& b : branches) {
            for (const LanePoint& p : b) {
                add16(out, uint16_t(int16_t(p.x))); add16(out, uint16_t(int16_t(p.y))); add16(out, uint16_t(int16_t(p.z)));
            }
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

void rush2::track1::set_snow_art(std::shared_ptr<const std::vector<uint8_t>> art) {
    uint64_t key = art_hash(art);
    std::lock_guard lock{ night_art_mutex };
    snow_art_bytes = std::move(art);
    snow_art_key = key;
}

std::shared_ptr<const std::vector<uint8_t>> rush2::track1::snow_art() {
    std::lock_guard lock{ night_art_mutex };
    return snow_art_bytes;
}

uint64_t rush2::track1::snow_art_hash() {
    std::lock_guard lock{ night_art_mutex };
    return snow_art_key;
}

void rush2::track1::set_night_art(std::shared_ptr<const std::vector<uint8_t>> art) {
    uint64_t key = art_hash(art);
    std::lock_guard lock{ night_art_mutex };
    night_art_bytes = std::move(art);
    night_art_key = key;
}

std::shared_ptr<const std::vector<uint8_t>> rush2::track1::night_art() {
    std::lock_guard lock{ night_art_mutex };
    return night_art_bytes;
}

uint64_t rush2::track1::night_art_hash() {
    std::lock_guard lock{ night_art_mutex };
    return night_art_key;
}

bool rush2::track1::convert_track(const std::vector<uint8_t>& rom, int t, const std::string& prefix,
                                  ConvertedTrack& out, std::string& error, rush2::diorama_look::Look look) {
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
        std::map<std::string, int> base_shifts;
        out.geometry = convert_geometry(rom, main, t, prefix, names, base_shifts, look);
        out.placement = convert_placement(rom, t, prefix, names, base_shifts, out.record_models);
        out.piece_models.clear();
        auto piece = [&](const std::string& rush2, const std::string& rush1) {
            auto it = names.find(rush1);
            if (it != names.end()) out.piece_models[rush2] = it->second;
        };
        for (const PieceSet& p : r1_pieces) {
            for (int k = 1; k <= p.count; k++) piece(p.rush2 + std::to_string(k), p.rush1 + std::to_string(k + p.shift));
        }
        for (int k = 1; k <= 4; k++) {
            piece("TREEHIT" + std::to_string(k) + "O1", "TREEHIT" + std::to_string(k) + "L1");
        }
        out.collision[0] = convert_collision(asset(rom, collision_asset + t));
        out.collision[1] = convert_collision(asset(rom, collision_back_asset + t));
        for (int b = 0; b < 2; b++) {
            out.path[b] = convert_path(rom, main, t, b != 0, out.demo_starts[b], out.lap_seconds[b], out.timing[b]);
        }
        {
            // Corners where the N64 lanes' speeds roll a Rush 2 car over (docs/rush1_research.md#lane-speed-caps): the
            // speed the cars tipped at in headless races (tools/drone_pace.py --race), capped about 10 mph under it.
            struct TrackCap { int track; bool backward; rush2::path_lane_fix::SpeedCap cap; };
            static const TrackCap caps[] = {
                // Downtown: the long left at (-650..-400, 210-270), rolled over at 96-107 mph (17 a race).
                { 3, false, { -540, 230, 140, 88 } },
                // Market: the steep banked turn down at (-8160..-8000, 1800-1860), rolled at 75-81 mph (6 a race).
                { 2, false, { -8100, 1815, 110, 70 } },
                // Sunset: the turn at (200, 540), rolled at 107 mph.
                { 5, false, { 200, 540, 100, 95 } },
            };
            for (const TrackCap& c : caps) {
                if (c.track == t) rush2::path_lane_fix::cap_speeds(out.path[c.backward ? 1 : 0], &c.cap, 1);
            }
            // Sunset backward: the lanes climb the curve at (1200..1450, 2350-2380) along its inside top edge, on the
            // steep bank above the road, and cars leaned onto it and rolled (10 a race at 93-100 mph; capping the speed
            // there to 82 only made them roll at 76-84, 28 a race): onto the road, 25 ft out.
            struct TrackFix { int track; bool backward; rush2::path_lane_fix::Fix fix; };
            static const TrackFix fixes[] = {
                { 5, true, { 0xF, 1340, 325, 2420, 25.0, 4, 6 } },
            };
            for (const TrackFix& f : fixes) {
                if (f.track == t) rush2::path_lane_fix::apply(out.path[f.backward ? 1 : 0], &f.fix, 1);
            }
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

bool rush2::track1::convert_container(const std::vector<uint8_t>& rom, int index, std::vector<uint8_t>& out,
                                      std::string& error) {
    try {
        Container c(asset(rom, index));
        Builder b;
        int s = b.add_source(c);
        // A model that doesn't convert is left out alone: the menu files' AAANULLOBJ placeholders hold a malformed
        // G_VTX. The texture table is left out: nothing in a record tells a load list from texels (+0x1C's high half
        // is a palette index), and texels read as a list run away; the models load their textures themselves.
        std::set<std::string> seen;
        std::string skipped;
        for (uint32_t i = 0; i < c.n_models; i++) {
            std::string name = c.name(i);
            if (!seen.insert(name.substr(0, 15)).second) continue;
            try {
                b.add_model(s, i, name);
                b.models.back().kind = s16(c.d, c.names_at + i * 0x18 + 20);
            }
            catch (const ConvertError& e) {
                skipped += " " + name + " (" + e.message + ")";
            }
        }
        // The palettes stay: a car's are its paint jobs (BMWREDD0...), which the game loads itself.
        for (uint32_t i = 0; i < c.n_pal; i++) {
            b.palettes.push_back({ c.palette_name(i), c.palette(i), s });
        }
        if (!skipped.empty()) printf("[Rush1] File %d: left out%s\n", index, skipped.c_str());
        out = b.build();
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
// are the track banners (tools/rush1/banners): Rush 1 shows its track names with its own models and fonts, which don't
// fit Rush 2's 128x32 logo slot.

namespace {
    const char* const diorama_models[rush2::track1::track_count] = { "TRK1O2", "TRK_2O2", "TRK_3O2", "TRK4O2", "TRK5O1",
                                                                     "TRK6O3", "TRK7O5" };
    constexpr int logo_w = 128, logo_h = 32;
    constexpr float diorama_size = 35.0f;   // Radius x scale of the stock dioramas, on average.

    // 128x32 CI8 texels (rows bottom-up, as Rush 2 stores its logos) and a 256-entry RGBA5551 palette: the track's
    // banner (include/track1_banners.h, tools/build_banners.py).
    void build_logo(int t, Bytes& texels, Bytes& palette) {
        texels.assign(logo_w * logo_h, 0);
        for (int y = 0; y < logo_h; y++) {
            memcpy(&texels[size_t((logo_h - 1 - y) * logo_w)], rush2::track1::banners::texels[t] + y * logo_w, logo_w);
        }
        palette.clear();
        for (int i = 0; i < 256; i++) add16(palette, rush2::track1::banners::palettes[t][i]);
    }

    std::optional<uint32_t> find_model(const Container& c, const std::string& name) {
        for (uint32_t i = 0; i < c.n_models; i++) {
            if (c.name(i) == name) return i;
        }
        return std::nullopt;
    }

    // The version band (rush2::track2049::version_ring_*, as the Rush 2049 previews' rings): a model `name` that draws
    // the diorama `base` (a copy of its list) and a band of color `rgb` round the foot of its sides. The band follows
    // the outline of the diorama's base from above (the union of its lowest triangles, concave bays and all, pushed out
    // a little), not a circle. The track select shows it for a course with another version installed (track2049_menu.cpp).
    // A SNOW or NIGHT version's model also wears the condition (`look`, dress_look below).
    using rush2::diorama_look::Look;
    struct Quad {
        std::array<std::array<double, 3>, 4> p;
        rush2::diorama_look::Rgb color;
        bool flake = false;     // a snowflake: drawn between the tags Graphics > Falling Snow branches past (snowfall.h)
    };
    void dress_look(Builder& b, Body& body, Look look, double lo, double radius, std::vector<Quad>& quads);

    void add_band_model(Builder& b, const std::string& base, const std::string& name, uint32_t rgb, Look look = Look::day) {
        const Builder::ModelOut* model = nullptr;
        for (const Builder::ModelOut& m : b.models) if (m.name == base) model = &m;
        if (model == nullptr) fail("no model " + base);
        std::vector<std::array<double, 2>> ring;     // the band's outline; empty: no band (the plain diorama)
        // The diorama's triangles (its list run through a vertex buffer as the RSP would).
        std::vector<std::array<std::array<double, 3>, 3>> tris;
        double lo = 1e30, hi = -1e30;
        {
            const Body& body = *model->lods[0];
            std::array<std::array<double, 3>, 64> slots{};
            std::map<size_t, const Fixup*> at;
            for (const Fixup& f : body.fixups) at[f.pos] = &f;
            for (size_t o = 0; o + 8 <= body.bytes.size(); o += 8) {
                uint32_t w0 = u32(body.bytes, o), w1 = u32(body.bytes, o + 4);
                uint8_t op = uint8_t(w0 >> 24);
                if (op == 0x01) {
                    auto f = at.find(o + 4);
                    if (f == at.end() || f->second->ref.load) continue;
                    uint32_t n = (w0 >> 12) & 0xFF, end = (w0 >> 1) & 0x7F;
                    size_t addr = b.sources[size_t(f->second->ref.index)].second + f->second->ref.offset;
                    for (uint32_t k = 0; k < n && end >= n && end - n + k < slots.size(); k++) {
                        size_t v = addr + k * 16;
                        slots[end - n + k] = { double(s16(b.out, v)), double(s16(b.out, v + 2)), double(s16(b.out, v + 4)) };
                        lo = std::min(lo, slots[end - n + k][1]);
                        hi = std::max(hi, slots[end - n + k][1]);
                    }
                }
                auto tri = [&](uint32_t w) {
                    uint32_t i0 = ((w >> 16) & 0xFF) / 2, i1 = ((w >> 8) & 0xFF) / 2, i2 = (w & 0xFF) / 2;
                    if (i0 < slots.size() && i1 < slots.size() && i2 < slots.size()) tris.push_back({ slots[i0], slots[i1], slots[i2] });
                };
                if (op == 0x05) tri(w0 & 0xFFFFFF);
                if (op == 0x06) tri(w0 & 0xFFFFFF), tri(w1 & 0xFFFFFF);
            }
        }
        // The base's outline: the diorama seen from above, the union of all its triangles (they have no underside, and
        // their sides run its full height), concave bays and all, pushed out a little. None found: the plain diorama.
        constexpr double grid = 4.0;            // Clipper units per model unit
        double height = 0.0, base_radius = 0.0;
        auto largest = [](const Clipper2Lib::Paths64& paths) {
            size_t best = 0;
            for (size_t i = 1; i < paths.size(); i++) {
                if (std::abs(Clipper2Lib::Area(paths[i])) > std::abs(Clipper2Lib::Area(paths[best]))) best = i;
            }
            return best;
        };
        Clipper2Lib::Paths64 parts;
        for (const auto& t : tris) {
            Clipper2Lib::Path64 q;
            for (const auto& v : t) q.push_back(Clipper2Lib::Point64(int64_t(std::llround(v[0] * grid)), int64_t(std::llround(v[2] * grid))));
            if (Clipper2Lib::Area(q) != 0) parts.push_back(q);
        }
        Clipper2Lib::Paths64 shape = parts.empty() ? Clipper2Lib::Paths64{} : Clipper2Lib::Union(parts, Clipper2Lib::FillRule::NonZero);
        if (!shape.empty()) {
            const Clipper2Lib::Path64& outline = shape[largest(shape)];
            double radius = std::sqrt(std::abs(Clipper2Lib::Area(outline)) / 3.14159) / grid;
            base_radius = radius;
            // Pushed out 1.5% of the base's size so it doesn't fight the sides; as tall as 7% of it, from the foot up.
            height = radius * 0.07;
            Clipper2Lib::Paths64 grown = Clipper2Lib::InflatePaths({ outline }, radius * 0.015 * grid,
                                                                   Clipper2Lib::JoinType::Round, Clipper2Lib::EndType::Polygon);
            grown = Clipper2Lib::SimplifyPaths(grown, radius * 0.01 * grid);
            if (!grown.empty()) {
                for (const auto& pt : grown[largest(grown)]) ring.push_back({ double(pt.x) / grid, double(pt.y) / grid });
            }
        }
        if (ring.size() < 3) ring.clear();
        // Vertices (16 bytes: s16 x, y, z, flag, s, t, u8 r, g, b, a) and the list: up to 15 sides a load.
        Bytes verts;
        Builder::ModelOut m{ name, 1, {}, {}, {}, model->radius };
        // The diorama's commands copied in (up to its G_ENDDL; its vertices, texels and loads stay shared), not called:
        // the game's mirror walk (mirror_winding_walk_80077518) swaps the winding of every model's list and follows
        // G_DL with no mark of what it already swapped, so a call from each of the five bands swapped the diorama's
        // triangles six times in all, back to unmirrored. In a mirrored race (a circuit picks them) the map then faced
        // away and was culled, and the buildings showed their insides.
        Body body;
        {
            const Body& src = *model->lods[0];
            size_t end = 0;
            while (end + 8 <= src.bytes.size() && src.bytes[end] != 0xDF) end += 8;
            body.bytes.assign(src.bytes.begin(), src.bytes.begin() + end);
            for (const Fixup& f : src.fixups) {
                if (f.pos < end) body.fixups.push_back(f);
            }
        }
        std::vector<Quad> quads;     // the condition's own pieces (lit windows, snowflakes), drawn with the band
        if (look != Look::day && base_radius > 0) dress_look(b, body, look, lo, base_radius, quads);
        // Texture off, sync, opaque z-buffered without fog, shade color.
        for (uint32_t w : { 0xD7000000u, 0xFFFFFFFFu, 0xE7000000u, 0u, 0xE200001Cu, 0x0C192230u, 0xFCFFFFFFu, 0xFFFE7C38u }) {
            add32(body.bytes, w);
        }
        std::vector<std::pair<size_t, uint32_t>> loads;     // (body offset of a G_VTX's address, vertex byte offset)
        const size_t sides = ring.size();
        constexpr size_t per_load = 15;
        for (size_t s0 = 0; s0 < sides; s0 += per_load) {
            const size_t n = std::min(per_load, sides - s0);
            const uint32_t first = uint32_t(verts.size());
            for (size_t e = 0; e <= n; e++) {
                const auto& r = ring[(s0 + e) % sides];
                for (double y : { lo, lo + height }) {
                    add16(verts, uint16_t(int16_t(std::lround(r[0]))));
                    add16(verts, uint16_t(int16_t(std::lround(y))));
                    add16(verts, uint16_t(int16_t(std::lround(r[1]))));
                    add16(verts, 0);
                    add16(verts, 0);
                    add16(verts, 0);
                    verts.push_back(uint8_t(rgb >> 16));
                    verts.push_back(uint8_t(rgb >> 8));
                    verts.push_back(uint8_t(rgb));
                    verts.push_back(0xFF);
                }
            }
            const uint32_t count = uint32_t(2 * (n + 1));
            loads.push_back({ body.bytes.size() + 4, first });
            add32(body.bytes, 0x01000000 | (count << 12) | (count << 1));
            add32(body.bytes, 0);
            auto tri = [](uint32_t a, uint32_t b2, uint32_t c) { return (a * 2 << 16) | (b2 * 2 << 8) | (c * 2); };
            for (uint32_t e = 0; e < uint32_t(n); e++) {
                uint32_t b0 = 2 * e, t0 = b0 + 1, b1 = b0 + 2, t1 = b0 + 3;
                // Both windings: it shows whatever the culling and mirror state.
                add32(body.bytes, 0x06000000 | tri(b0, t0, t1));
                add32(body.bytes, tri(b0, t1, b1));
                add32(body.bytes, 0x06000000 | tri(b0, t1, t0));
                add32(body.bytes, tri(b0, b1, t1));
            }
        }
        // The condition's quads, 15 a load (60 vertices), both windings as the band: the roofs, then the snowflakes
        // between G_NOOPs tagged for Graphics > Falling Snow (include/snowfall.h), which the track select branches past
        // while it is off.
        std::vector<Quad> ordered;
        for (const Quad& q : quads) if (!q.flake) ordered.push_back(q);
        const size_t first_flake = ordered.size();
        for (const Quad& q : quads) if (q.flake) ordered.push_back(q);
        for (size_t q0 = 0; q0 < ordered.size(); ) {
            const size_t end = q0 < first_flake ? first_flake : ordered.size();
            if (q0 == first_flake) {
                add32(body.bytes, 0);                   // G_NOOP "SNOW"
                add32(body.bytes, rush2::snowfall::flakes_start_tag);
            }
            const size_t n = std::min(per_load, end - q0);
            const uint32_t first = uint32_t(verts.size());
            for (size_t k = 0; k < n; k++) {
                const Quad& q = ordered[q0 + k];
                for (const auto& p : q.p) {
                    for (int a = 0; a < 3; a++) add16(verts, uint16_t(int16_t(std::clamp(std::lround(p[a]), -32768L, 32767L))));
                    add16(verts, 0);
                    add16(verts, 0);
                    add16(verts, 0);
                    verts.push_back(uint8_t(q.color[0]));
                    verts.push_back(uint8_t(q.color[1]));
                    verts.push_back(uint8_t(q.color[2]));
                    verts.push_back(0xFF);
                }
            }
            const uint32_t count = uint32_t(4 * n);
            loads.push_back({ body.bytes.size() + 4, first });
            add32(body.bytes, 0x01000000 | (count << 12) | (count << 1));
            add32(body.bytes, 0);
            auto tri = [](uint32_t a, uint32_t b2, uint32_t c) { return (a * 2 << 16) | (b2 * 2 << 8) | (c * 2); };
            for (uint32_t k = 0; k < uint32_t(n); k++) {
                uint32_t v = 4 * k;
                add32(body.bytes, 0x06000000 | tri(v, v + 1, v + 2));
                add32(body.bytes, tri(v, v + 2, v + 3));
                add32(body.bytes, 0x06000000 | tri(v, v + 2, v + 1));
                add32(body.bytes, tri(v, v + 3, v + 2));
            }
            q0 += n;
            if (q0 == ordered.size() && first_flake < ordered.size()) {
                add32(body.bytes, 0);                   // G_NOOP "SNOE"
                add32(body.bytes, rush2::snowfall::flakes_end_tag);
            }
        }
        add32(body.bytes, 0xD7000002);                  // texture on, as the lists end
        add32(body.bytes, 0xFFFFFFFF);
        for (uint32_t w : restore_state) add32(body.bytes, w);
        add32(body.bytes, 0xDF000000);
        add32(body.bytes, 0);
        int raw = b.add_raw(verts);
        for (const auto& [at, off] : loads) body.fixups.push_back({ at, { false, raw, off } });
        m.lods[0] = std::move(body);
        m.dist[0] = model->dist[0];
        m.flags[0] = model->flags[0];
        b.models.push_back(std::move(m));
    }

    // A SNOW or NIGHT version's diorama (docs/sfrush_ps1.md#snow-and-night-dioramas): body (a copy of the diorama's
    // commands) is pointed at recolored copies of its vertices and TLUTs, and `quads` gets the condition's pieces.
    // Snow: the up-facing ground's textures go snow white, the rest a little colder, the buildings' roofs (drawn with
    // their window textures) are redrawn as snow, and flakes float over the island. Night: the PS1's night darkening
    // (bluer, the route kept lit), and lights in the textures themselves (copied texels pointing at a spare TLUT entry
    // set to window light): some of the buildings' window texels, and scattered texels of the city ground.
    void dress_look(Builder& b, Body& body, Look look, double lo, double radius, std::vector<Quad>& quads) {
        namespace dl = rush2::diorama_look;
        using V = std::array<double, 3>;
        std::map<size_t, const Fixup*> at;
        for (const Fixup& f : body.fixups) at[f.pos] = &f;
        auto source = [&](const Fixup& f) { return b.sources[size_t(f.ref.index)].second + f.ref.offset; };

        // Pass 1: the vertices each load reads, copied, and how far each copy faces up (the most of the triangles that
        // use it); the walls of textured triangles above the ground (the buildings) for the windows.
        Bytes copy;
        std::vector<std::pair<size_t, uint32_t>> moved;      // (body offset of the address word, offset in copy)
        std::vector<double> up;                              // per copied vertex
        std::vector<uint8_t> textured;                       // per copied vertex: drawn by a textured triangle
        std::vector<V> pos;
        // Per TLUT (body offset of its SETTIMG's address word): triangles drawn with it, and how many face up.
        std::map<size_t, std::pair<int, int>> tlut_use;
        size_t tlut = 0, last_timg = 0;
        bool texture_on = true;                              // the game draws models with texturing on
        // Per texel load (body offset of its SETTIMG's address word): its render tile, the TLUT it is drawn with and
        // how its triangles face.
        struct TexUse {
            size_t tlut = 0;
            bool tluts = false;                              // drawn with more than one TLUT
            uint32_t fmt = 0, siz = 0, line = 0, w = 0, h = 0, bytes = 0;
            int tris = 0, up = 0, walls = 0;
        };
        std::map<size_t, TexUse> tex_use;
        std::map<size_t, uint32_t> timg_siz;
        size_t cur_tex = 0;
        struct Tile { uint32_t fmt = 0, siz = 0, line = 0, w = 0, h = 0; } tile0;
        std::array<int, 64> slot{};
        slot.fill(-1);
        // A triangle: corners, normal, mean shade, the texture it is drawn with (0: none) and where its indices are
        // (body offset of the word, 0 for G_TRI1's / G_TRI2's first, 1 for G_TRI2's second).
        struct Tri { V a, b, c, n; dl::Rgb color; size_t tex; size_t at; };
        std::vector<Tri> tris;
        for (size_t o = 0; o + 8 <= body.bytes.size(); o += 8) {
            uint32_t w0 = u32(body.bytes, o), w1 = u32(body.bytes, o + 4);
            uint8_t op = uint8_t(w0 >> 24);
            if (op == 0xD7) texture_on = (w0 & 2) != 0;
            if (op == 0xFD) {
                last_timg = o + 4;
                timg_siz[o + 4] = (w0 >> 19) & 3;
            }
            if (op == 0xF0) tlut = last_timg;
            if (op == 0xF3) {
                cur_tex = last_timg;
                tex_use[cur_tex].bytes = ((((w1 >> 12) & 0xFFF) + 1) * (4u << timg_siz[cur_tex])) / 8;
            }
            // The render tile (tile 0) as it stands: a list sets it only when it changes, so a texture takes the
            // settings in force when its triangles are drawn.
            if (op == 0xF5 && ((w1 >> 24) & 7) == 0) {
                tile0.fmt = (w0 >> 21) & 7;
                tile0.siz = (w0 >> 19) & 3;
                tile0.line = (w0 >> 9) & 0x1FF;
            }
            if (op == 0xF2 && ((w1 >> 24) & 7) == 0) {
                tile0.w = ((w1 >> 12) & 0xFFF) / 4 - ((w0 >> 12) & 0xFFF) / 4 + 1;
                tile0.h = (w1 & 0xFFF) / 4 - (w0 & 0xFFF) / 4 + 1;
            }
            if (op == 0x01) {
                auto f = at.find(o + 4);
                if (f == at.end() || f->second->ref.load) continue;
                uint32_t n = (w0 >> 12) & 0xFF, end = (w0 >> 1) & 0x7F;
                size_t from = source(*f->second);
                moved.push_back({ o + 4, uint32_t(copy.size()) });
                for (uint32_t k = 0; k < n; k++) {
                    copy.insert(copy.end(), b.out.begin() + from + k * 16, b.out.begin() + from + k * 16 + 16);
                    pos.push_back({ double(s16(b.out, from + k * 16)), double(s16(b.out, from + k * 16 + 2)),
                                    double(s16(b.out, from + k * 16 + 4)) });
                    up.push_back(0.0);
                    textured.push_back(0);
                    if (end >= n && end - n + k < slot.size()) slot[end - n + k] = int(pos.size() - 1);
                }
            }
            auto tri = [&](uint32_t w, size_t where) {
                int i0 = slot[((w >> 16) & 0xFF) / 2 % 64], i1 = slot[((w >> 8) & 0xFF) / 2 % 64], i2 = slot[(w & 0xFF) / 2 % 64];
                if (i0 < 0 || i1 < 0 || i2 < 0) return;
                const V& a = pos[size_t(i0)];
                const V& b2 = pos[size_t(i1)];
                const V& c = pos[size_t(i2)];
                V e1{ b2[0] - a[0], b2[1] - a[1], b2[2] - a[2] }, e2{ c[0] - a[0], c[1] - a[1], c[2] - a[2] };
                V n{ e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0] };
                double l = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
                if (l < 1e-9) return;
                n = { n[0] / l, n[1] / l, n[2] / l };
                for (int i : { i0, i1, i2 }) {
                    up[size_t(i)] = std::max(up[size_t(i)], std::abs(n[1]));
                    if (texture_on) textured[size_t(i)] = 1;
                }
                if (texture_on && tlut != 0) {
                    auto& u = tlut_use[tlut];
                    u.first++;
                    if (std::abs(n[1]) > 0.7) u.second++;
                }
                if (texture_on && cur_tex != 0) {
                    TexUse& u = tex_use[cur_tex];
                    if (u.tlut != 0 && u.tlut != tlut) u.tluts = true;
                    if (u.tris > 0 && (u.fmt != tile0.fmt || u.siz != tile0.siz || u.line != tile0.line || u.h != tile0.h)) {
                        u.tluts = true;                      // drawn two ways: left alone
                    }
                    u.tlut = tlut;
                    u.fmt = tile0.fmt;
                    u.siz = tile0.siz;
                    u.line = tile0.line;
                    u.w = tile0.w;
                    u.h = tile0.h;
                    u.tris++;
                    if (std::abs(n[1]) > 0.7) u.up++;
                    if (std::abs(n[1]) < 0.3) u.walls++;
                }
                dl::Rgb color{ 0, 0, 0 };
                for (int i : { i0, i1, i2 }) for (int k = 0; k < 3; k++) color[size_t(k)] += copy[size_t(i) * 16 + 12 + size_t(k)] / 3;
                tris.push_back({ a, b2, c, n, color, texture_on ? cur_tex : 0, where });
            };
            if (op == 0x05) tri(w0 & 0xFFFFFF, o);
            if (op == 0x06) tri(w0 & 0xFFFFFF, o), tri(w1 & 0xFFFFFF, o + 4);
        }
        if (pos.empty()) return;
        // The ground's height: the lower third of the up-facing triangles (the island's top; roofs are fewer and higher).
        std::vector<double> flat;
        for (const Tri& t : tris) if (std::abs(t.n[1]) > 0.8) flat.push_back((t.a[1] + t.b[1] + t.c[1]) / 3);
        std::sort(flat.begin(), flat.end());
        const double ground = flat.empty() ? lo : flat[flat.size() / 3];
        const uint32_t seed = look == Look::snow ? 0x5A0Bu : 0x7E1Fu;

        // Vertex colors. A textured triangle's color is its texel times its shade, so where the TLUT carries the
        // condition (below) the shade is left as it is: night darkened twice was near black in game.
        for (size_t v = 0; v < pos.size(); v++) {
            uint8_t* c = &copy[v * 16 + 12];
            dl::Rgb rgb{ c[0], c[1], c[2] }, out;
            if (dl::is_route_red(rgb)) continue;
            double w = std::clamp((up[v] - 0.55) / 0.35, 0.0, 1.0);
            if (textured[v]) {
                if (look == Look::night) continue;
                out = dl::mix(rgb, { 255, 255, 255 }, w * 0.5);     // snow: shaded slopes a little lighter
            }
            else if (look == Look::night) {
                out = dl::night(rgb);
            }
            else {
                // Snow lies on what faces up, in patches about a tenth of the island across.
                double patch = dl::value_noise(pos[v][0] / (radius * 0.2), pos[v][2] / (radius * 0.2), seed);
                double tone = dl::value_noise(pos[v][0] / (radius * 0.08), pos[v][2] / (radius * 0.08), seed + 1);
                out = dl::mix(dl::snow_cold(rgb), dl::snow_white(tone), w * (0.55 + 0.45 * patch));
            }
            c[0] = uint8_t(out[0]);
            c[1] = uint8_t(out[1]);
            c[2] = uint8_t(out[2]);
        }
        // The CI textures by TLUT load, read back, and which are the buildings' (a wall color with a grid of darker
        // window texels: two or three entries, the darkest 5-45% of the texels, drawn mostly on walls) and which the
        // city ground's (a large grey texture drawn on what faces up).
        auto tlut_count = [&](size_t timg) {                 // entries the LOADTLUT after SETTIMG `timg` loads
            for (size_t k = timg + 4; k + 8 <= body.bytes.size() && body.bytes[k] != 0xFD; k += 8) {
                if (body.bytes[k] == 0xF0) return ((u32(body.bytes, k + 4) >> 14) & 0x3FF) + 1;
            }
            return 0u;
        };
        auto tlut_color = [&](size_t timg, uint32_t k) {
            size_t from = source(*at.at(timg));
            uint16_t e = uint16_t(b.out[from + 2 * k] << 8 | b.out[from + 2 * k + 1]);
            auto c5 = [](int v) { return (v << 3) | (v >> 2); };
            return dl::Rgb{ c5((e >> 11) & 31), c5((e >> 6) & 31), c5((e >> 1) & 31) };
        };
        struct Indexed { size_t timg; const TexUse* use; std::vector<uint8_t> idx; uint32_t w = 0; bool building = false, city = false; uint8_t dark = 0; };
        std::map<size_t, std::vector<Indexed>> by_tlut;
        std::set<size_t> mixed;                              // TLUT loads with a texture this can't read
        std::set<size_t> buildings;                          // texel loads of the buildings' textures
        for (const auto& [timg, u] : tex_use) {
            if (u.tlut == 0) continue;
            auto f = at.find(timg);
            const uint32_t bits = 4u << u.siz;
            const uint32_t w = u.line ? u.line * 64 / bits : 0;
            if (u.tluts || u.fmt != 2 || u.siz > 1 || f == at.end() || f->second->ref.load || w == 0 || u.h == 0 ||
                size_t(w) * u.h * bits / 8 > u.bytes || at.find(u.tlut) == at.end() || tlut_count(u.tlut) == 0) {
                mixed.insert(u.tlut);
                continue;
            }
            size_t from = source(*f->second);
            const uint32_t count = tlut_count(u.tlut);
            if (from + size_t(w) * u.h * bits / 8 > b.out.size() || source(*at.at(u.tlut)) + count * 2 > b.out.size()) {
                mixed.insert(u.tlut);
                continue;
            }
            Indexed t{ timg, &u, {}, w };
            for (uint32_t i = 0; i < w * u.h; i++) {
                t.idx.push_back(bits == 8 ? b.out[from + i] : uint8_t((b.out[from + i / 2] >> (i % 2 ? 0 : 4)) & 15));
            }
            std::map<uint8_t, int> n;
            bool range = true;
            for (uint8_t i : t.idx) { n[i]++; range = range && i < count; }
            if (!range) { mixed.insert(u.tlut); continue; }
            double dark_l = 1e9;
            uint8_t main = 0;
            int main_n = -1;
            for (const auto& [i, c] : n) {
                double l = dl::luminance(tlut_color(u.tlut, i));
                if (l < dark_l) { dark_l = l; t.dark = i; }
                if (c > main_n) { main_n = c; main = i; }
            }
            const double share = double(n[t.dark]) / t.idx.size();
            t.building = u.walls * 10 >= u.tris * 7 && n.size() >= 2 && n.size() <= 3 && share >= 0.05 && share <= 0.45 &&
                         dl::luminance(tlut_color(u.tlut, main)) - dark_l > 30;
            double sat = 0;
            for (uint8_t i : t.idx) {
                dl::Rgb c = tlut_color(u.tlut, i);
                sat += std::max({ c[0], c[1], c[2] }) - std::min({ c[0], c[1], c[2] });
            }
            sat /= double(t.idx.size());
            t.city = u.up * 10 >= u.tris * 7 && t.idx.size() >= 512 && sat < 40;
            if (t.building) buildings.insert(timg);
            by_tlut[u.tlut].push_back(std::move(t));
        }

        // Night lights: a building texture gets about 45% of its window texels lit, the city ground about 6% of its
        // texels, scattered. The lit texels are copies pointing at a TLUT entry none of that TLUT's textures uses,
        // which the TLUT copy below sets to window light (no spare entry: no lights in that TLUT's textures).
        std::map<size_t, uint32_t> lit_entry;                // TLUT load -> its spare entry, set to window light
        if (look == Look::night) {
            for (auto& [timg, texs] : by_tlut) {
                const uint32_t count = tlut_count(timg);
                if (mixed.count(timg) || count == 0 || count > 256) continue;
                std::vector<bool> used(count, false);
                for (const Indexed& t : texs) for (uint8_t i : t.idx) used[i] = true;
                uint32_t spare = count;
                for (uint32_t k = 0; k < count && spare == count; k++) if (!used[k]) spare = k;
                if (spare == count) continue;
                bool any = false;
                for (const Indexed& t : texs) {
                    if (!t.building && !t.city) continue;
                    const TexUse& u = *t.use;
                    std::vector<uint8_t> idx = t.idx;
                    for (uint32_t i = 0; i < idx.size(); i++) {
                        double r = dl::hash01(int64_t(i % t.w), int64_t(i / t.w) * 131 + int64_t(t.timg % 977), seed);
                        if (t.building ? (idx[i] == t.dark && r < 0.45) : r < 0.06) idx[i] = uint8_t(spare);
                    }
                    const uint32_t bits = 4u << u.siz;
                    moved.push_back({ t.timg, uint32_t(copy.size()) });
                    if (bits == 8) {
                        copy.insert(copy.end(), idx.begin(), idx.end());
                    } else {
                        for (size_t i = 0; i < idx.size(); i += 2) {
                            copy.push_back(uint8_t((idx[i] << 4) | (i + 1 < idx.size() ? idx[i + 1] : 0)));
                        }
                    }
                    // The rest of the load (a block longer than the tile) as it was.
                    size_t from = source(*at.at(t.timg)), have = idx.size() * bits / 8;
                    copy.insert(copy.end(), b.out.begin() + from + have, b.out.begin() + from + u.bytes);
                    while (copy.size() % 8) copy.push_back(0);
                    any = true;
                }
                if (any) lit_entry[timg] = spare;
            }
        }

        // Snow on the roofs: the buildings' roofs (up-facing triangles above the ground whose corners are all corners
        // of the buildings' walls, the triangles drawn with a window texture) are left out of the list (their indices
        // made one vertex, so they draw nothing) and redrawn with the band as snow: the PS1's snow white, a tone per
        // roof, darkened as the roof's own shade is.
        if (look == Look::snow) {
            std::set<std::array<long, 3>> wall_corners;
            auto key = [](const V& p) { return std::array<long, 3>{ std::lround(p[0]), std::lround(p[1]), std::lround(p[2]) }; };
            for (const Tri& t : tris) {
                if (t.tex == 0 || !buildings.count(t.tex)) continue;
                for (const V* p : { &t.a, &t.b, &t.c }) wall_corners.insert(key(*p));
            }
            for (const Tri& t : tris) {
                if (std::abs(t.n[1]) <= 0.7 || dl::is_route_red(t.color)) continue;
                if (!wall_corners.count(key(t.a)) || !wall_corners.count(key(t.b)) || !wall_corners.count(key(t.c))) continue;
                if (std::min({ t.a[1], t.b[1], t.c[1] }) <= ground + radius * 0.005) continue;
                body.bytes[t.at + 1] = body.bytes[t.at + 3];
                body.bytes[t.at + 2] = body.bytes[t.at + 3];
                Quad roof;
                roof.p = { t.a, t.b, t.c, t.c };
                double tone = dl::hash01(std::llround(t.a[0] / 8), std::llround(t.a[2] / 8), seed);
                double shade = std::clamp(dl::luminance(t.color) / 230.0, 0.75, 1.0);
                dl::Rgb c = dl::snow_white(0.55 + 0.45 * tone);
                roof.color = { int(c[0] * shade), int(c[1] * shade), int(c[2] * shade) };
                quads.push_back(roof);
            }
        }

        // The TLUTs: every SETTIMG a LOADTLUT reads (before the next SETTIMG), its RGBA5551 entries copied and recolored.
        for (size_t o = 0; o + 8 <= body.bytes.size(); o += 8) {
            if (body.bytes[o] != 0xFD) continue;
            auto f = at.find(o + 4);
            if (f == at.end() || f->second->ref.load) continue;
            uint32_t count = 0;
            for (size_t k = o + 8; k + 8 <= body.bytes.size() && body.bytes[k] != 0xFD; k += 8) {
                if (body.bytes[k] == 0xF0) { count = ((u32(body.bytes, k + 4) >> 14) & 0x3FF) + 1; break; }
            }
            if (count == 0 || count > 256) continue;
            size_t from = source(*f->second);
            if (from + count * 2 > b.out.size()) continue;
            moved.push_back({ o + 4, uint32_t(copy.size()) });
            // Snow: a texture mostly drawn on what faces up (the ground's gravel and grass) goes snow white, its light
            // and dark kept as lighter and darker snow (water blue stays water); the others (walls, the base's earth) are
            // frosted a little.
            auto use = tlut_use.find(o + 4);
            const bool ground = use != tlut_use.end() && use->second.second * 2 >= use->second.first;
            auto lit = lit_entry.find(o + 4);
            for (uint32_t k = 0; k < count; k++) {
                uint16_t e = uint16_t(b.out[from + 2 * k] << 8 | b.out[from + 2 * k + 1]);
                auto c5 = [](int v) { return (v << 3) | (v >> 2); };
                dl::Rgb rgb{ c5((e >> 11) & 31), c5((e >> 6) & 31), c5((e >> 1) & 31) }, out = rgb;
                if (lit != lit_entry.end() && k == lit->second) {
                    out = dl::window_light(0.3);
                } else if (!dl::is_route_red(rgb)) {
                    if (look == Look::night) out = dl::night(rgb);
                    else if (ground && !(rgb[2] > rgb[0] * 1.3 + 8 && rgb[2] > rgb[1] * 1.05)) out =dl::mix(dl::snow_cold(rgb), dl::snow_white(0.3 + 0.7 * dl::luminance(rgb) / 255), 0.8);
                    else out = dl::mix(dl::snow_cold(rgb), dl::snow_white(0.7), 0.3);
                }
                // The lit entry was unused, often 0 (transparent): it is made opaque.
                const bool lit_here = lit != lit_entry.end() && k == lit->second;
                uint16_t ne = uint16_t(((out[0] >> 3) << 11) | ((out[1] >> 3) << 6) | ((out[2] >> 3) << 1) | (lit_here ? 1 : (e & 1)));
                copy.push_back(uint8_t(ne >> 8));
                copy.push_back(uint8_t(ne));
            }
            while (copy.size() % 8) copy.push_back(0);
        }
        int raw = b.add_raw(copy);
        for (const auto& [pos_in_body, off] : moved) {
            for (Fixup& f : body.fixups) {
                if (f.pos == pos_in_body) f.ref = { false, raw, off };
            }
        }

        if (look == Look::snow) {
            // Flakes over the island, as in a snow globe: three crossed squares each (one always faces the camera
            // enough), between the ground and half the island's radius above it.
            double x0 = 1e30, x1 = -1e30, z0 = 1e30, z1 = -1e30;
            for (const V& p : pos) {
                x0 = std::min(x0, p[0]); x1 = std::max(x1, p[0]);
                z0 = std::min(z0, p[2]); z1 = std::max(z1, p[2]);
            }
            const double size = radius * 0.011;
            int placed = 0;
            for (int k = 0; k < 2000 && placed < 160; k++) {
                double x = x0 + (x1 - x0) * dl::hash01(k, 1, seed), z = z0 + (z1 - z0) * dl::hash01(k, 2, seed);
                double y = ground + radius * (0.04 + 0.5 * dl::hash01(k, 3, seed));
                // Over the island only: near some vertex of it.
                bool over = false;
                for (size_t v = 0; v < pos.size() && !over; v += 3) {
                    over = std::abs(pos[v][0] - x) < radius * 0.12 && std::abs(pos[v][2] - z) < radius * 0.12;
                }
                if (!over) continue;
                placed++;
                dl::Rgb color = dl::snow_white(0.75 + 0.25 * dl::hash01(k, 4, seed));
                for (int plane = 0; plane < 3; plane++) {
                    Quad f;
                    for (int c = 0; c < 4; c++) {
                        double u = (c == 0 || c == 3) ? -size : size, v = c < 2 ? -size : size;
                        V p{ x, y, z };
                        if (plane == 0) { p[0] += u; p[1] += v; }
                        else if (plane == 1) { p[2] += u; p[1] += v; }
                        else { p[0] += u; p[2] += v; }
                        f.p[size_t(c)] = p;
                    }
                    f.color = color;
                    f.flake = true;
                    quads.push_back(f);
                }
            }
        }
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
            const std::string model = "R1TRACK" + std::to_string(t + 1);
            b.add_model(s, *i, model, false);
            // Its version bands (shown for a course with another version: the SF Rush arcade tracks', the PlayStation
            // SF Rush's SNOW, NIGHT and STUNT): N64, ARCADE, SNOW, NIGHT and STUNT.
            add_band_model(b, model, model + "N", rush2::track2049::version_ring_n64);
            add_band_model(b, model, model + "A", rush2::track2049::version_ring_arcade);
            add_band_model(b, model, model + "S", rush2::track2049::version_ring_snow, Look::snow);
            add_band_model(b, model, model + "K", rush2::track2049::version_ring_night, Look::night);
            add_band_model(b, model, model + "T", rush2::track2049::version_ring_stunt);
            Bytes texels, palette;
            build_logo(t, texels, palette);
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

void rush2::track1::add_banner_images() {
    // The banners as the game stores them: rows bottom-up.
    for (int t = 0; t < track_count; t++) {
        std::vector<uint8_t> indices(logo_w * logo_h);
        rush2::upscale::Image image;
        image.width = logo_w;
        image.height = logo_h;
        image.rgba.resize(size_t(logo_w * logo_h * 4));
        for (int y = 0; y < logo_h; y++) {
            int src = (logo_h - 1 - y) * logo_w;
            memcpy(&indices[size_t(y * logo_w)], banners::texels[t] + src, logo_w);
            for (int x = 0; x < logo_w; x++) {
                uint32_t c = banners::rgba[t][src + x];
                uint8_t* p = &image.rgba[size_t((y * logo_w + x) * 4)];
                p[0] = uint8_t(c >> 24); p[1] = uint8_t(c >> 16); p[2] = uint8_t(c >> 8); p[3] = uint8_t(c);
            }
        }
        rush2::upscale::add_exact_image(std::move(indices), std::move(image));
    }
}

void rush2::track1::course_logo(int t, std::vector<uint8_t>& texels, std::vector<uint8_t>& palette, std::vector<uint8_t>& rgba) {
    texels.clear(), palette.clear(), rgba.clear();
    if (t < 0 || t >= track_count) return;
    Bytes tb, pb;
    build_logo(t, tb, pb);
    texels.assign(tb.begin(), tb.end());
    palette.assign(pb.begin(), pb.end());
    // Full color in the texels' row order (bottom-up), as the banners are drawn (add_banner_images).
    rgba.resize(size_t(logo_w * logo_h * 4));
    for (int y = 0; y < logo_h; y++) {
        int src = (logo_h - 1 - y) * logo_w;
        for (int x = 0; x < logo_w; x++) {
            uint32_t c = banners::rgba[t][src + x];
            uint8_t* p = &rgba[size_t((y * logo_w + x) * 4)];
            p[0] = uint8_t(c >> 24); p[1] = uint8_t(c >> 16); p[2] = uint8_t(c >> 8); p[3] = uint8_t(c);
        }
    }
}

bool rush2::track1::build_race_logo(const std::vector<uint8_t>& logo, const std::vector<uint8_t>& rom, int t,
                                    std::vector<uint8_t>& out) {
    try {
        Bytes t_bytes, p_bytes;
        build_logo(t, t_bytes, p_bytes);
        return rush2::track2049::set_race_logo(logo, t_bytes, p_bytes, out);
    }
    catch (const ConvertError&) {
        return false;
    }
}
