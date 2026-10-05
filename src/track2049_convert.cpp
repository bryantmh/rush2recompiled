// Converts a Rush 2049 race track into the files of a Rush 2 track slot. This is the C++ port of the prototype in
// tools/rush2049 (track.py and the parts of model.py, placement.py, collision.py and paths.py it calls) and gives
// byte-identical output. Format details and evidence are in docs/rush2049_research (geometry.md, placement.md,
// collision.md, race.md). All data is big-endian. 2049 track k (= 2049 track id + 1: race tracks 1-6, stunt arenas
// 15-18) uses these 2049 files:
//
// Geometry (100+k track, 81+k track objects, 78 shared flags/triggers, 68 coins -> one Rush 2 model container)
//     2049 container: word 0 = offset of a directory of {tag, offset, size or count} entries. IMAG texels, TXLD
//     texture-load lists, OBJS object lists and vertices, OBHD 0x58-byte object records {name[16], f32 radius,
//     u16 kind, s16 lod count, {u16 texture, u16 flags, f32 distance, u32 list, u32 vertices}[4]}, TXHD 0x24-byte
//     texture records (the Rush 2 record + a word), PLHD 0x18-byte palette records (= Rush 2's). Pointers in TXLD
//     and TXHD/PLHD are IMAG-relative, the rest file-relative.
//     Rush 2 container: 10-word header {model table, name table, texture table, palette table, model count, texture
//     count, palette count, texture-load lists start, end, 0}, models 0x34 {u32 lod count, {u16 texture, u16 flags,
//     f32 distance, u32 list}[4]}, names 0x18 {name[16], f32 radius, u16 kind, u16 0} sorted for a binary search
//     over 15 characters, textures 0x20, palettes 0x18. Rush 2 rebases one texture-load list range with a linear
//     scan, so the merged file groups all IMAG chunks, then all TXLD chunks, then all OBJS chunks, and moves every
//     pointer to the new position of what it points into. Lists that leave the texture LUT mode other than RGBA16
//     get a copy ending in a restore (Rush 2 sets it once per frame).
// Placement (119+k -> Rush 2 placement tree)
//     Records are Rush 2's 100-byte records plus a dynamic-object id. 2049 dynamic objects are classified by 2049's
//     own type table (main data 0x80117530) and kept, mapped to a Rush 2 breakable name, placed as their static
//     model or dropped. Children of a dropped record move up. Kept models under a parent are made parent-relative.
//     Coins become Rush 2 key records (KEYS0-7 silver, KEYG0-7 gold; src/collectibles.cpp). Knock-over props, signs
//     and cacti keep their 2049 models, renamed X49<model> so Rush 2's prefix classifier doesn't take CONE1G1 or
//     STOPHITG1 for its own breakables, and are listed as prop records for src/track2049_props.cpp.
// Collision (138+k): Rush 2049's layout with a 0x10-byte header, a MOVER section and 32-bit leaf offsets, rewritten
//     as Rush 2's; if the leaf section doesn't fit 16-bit offsets, bottom quadtree nodes are merged.
// AI paths (157+k forward, 176+k backward; a stunt arena's one path, 157+k as 2049's loader picks it although the
//     editor names inside the files run the other way, serves both): same format in both games;
//     validated and kept as they are, except that a stunt arena's stub AI lanes are replaced with its spine. Stunt arenas have no per-track object file (81+k).
// PVS, fog colour: per-track tables in 2049's main data, which is raw deflate at ROM 0xB0CB10, loaded at 0x80086A50.
//
// Matching the Python: floats are computed in double and rounded to float when written, as struct.pack('>f') does;
// Python's sum() of floats is compensated (Neumaier), so py_sum replicates it; names sort as unsigned bytes, which is
// how std::string compares. FP contraction is off so no fused multiply-add changes a result.

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <array>
#include <functional>
#include <map>
#include <optional>
#include <queue>
#include <unordered_map>
#include <unordered_set>

#include "assets.h"
#include "rush2049_rom.h"
#include "track2049_convert.h"

#if defined(__clang__)
#pragma clang fp contract(off)
#elif defined(_MSC_VER)
#pragma fp_contract(off)
#endif

namespace {
    using Bytes = std::vector<uint8_t>;

    // Thrown by the converters; convert_track turns it into its error message.
    struct ConvertError {
        std::string message;
    };

    [[noreturn]] void fail(const std::string& message) {
        throw ConvertError{ message };
    }

    std::string hex(uint64_t v) {
        char buf[24];
        snprintf(buf, sizeof(buf), "0x%llX", (unsigned long long)v);
        return buf;
    }

    // ------------------------------------------------------------------------------------------------------------
    // Big-endian access. Reads fail on data that ends early, as Python's struct and indexing do.

    void need(const Bytes& d, size_t offset, size_t size) {
        if (offset > d.size() || size > d.size() - offset) {
            fail("read of " + std::to_string(size) + " bytes at " + hex(offset) + " past the end (" + hex(d.size()) + ")");
        }
    }

    uint8_t u8(const Bytes& d, size_t o) {
        need(d, o, 1);
        return d[o];
    }

    uint16_t u16(const Bytes& d, size_t o) {
        need(d, o, 2);
        return (uint16_t)((d[o] << 8) | d[o + 1]);
    }

    int16_t s16(const Bytes& d, size_t o) {
        return (int16_t)u16(d, o);
    }

    uint32_t u32(const Bytes& d, size_t o) {
        need(d, o, 4);
        return ((uint32_t)d[o] << 24) | ((uint32_t)d[o + 1] << 16) | ((uint32_t)d[o + 2] << 8) | d[o + 3];
    }

    double f32(const Bytes& d, size_t o) {
        uint32_t bits = u32(d, o);
        float f;
        memcpy(&f, &bits, 4);
        return f;
    }

    uint32_t float_bits(double v) {
        float f = (float)v;
        uint32_t bits;
        memcpy(&bits, &f, 4);
        return bits;
    }

    void put32(Bytes& d, size_t o, uint32_t v) {
        need(d, o, 4);
        d[o] = (uint8_t)(v >> 24);
        d[o + 1] = (uint8_t)(v >> 16);
        d[o + 2] = (uint8_t)(v >> 8);
        d[o + 3] = (uint8_t)v;
    }

    void add16(Bytes& d, uint32_t v) {
        d.push_back((uint8_t)(v >> 8));
        d.push_back((uint8_t)v);
    }

    void add32(Bytes& d, uint32_t v) {
        d.push_back((uint8_t)(v >> 24));
        d.push_back((uint8_t)(v >> 16));
        d.push_back((uint8_t)(v >> 8));
        d.push_back((uint8_t)v);
    }

    void add_float(Bytes& d, double v) {
        add32(d, float_bits(v));
    }

    // A 16-byte name field: the name cut to 15 characters, NUL padded.
    void add_name(Bytes& d, const std::string& name) {
        size_t n = std::min<size_t>(name.size(), 15);
        d.insert(d.end(), name.begin(), name.begin() + n);
        d.insert(d.end(), 16 - n, 0);
    }

    void align8(Bytes& d) {
        while (d.size() & 7) {
            d.push_back(0);
        }
    }

    // The characters of d[o, o+n) up to the first NUL (clipped to the data, like a Python slice).
    std::string cstr(const Bytes& d, size_t o, size_t n) {
        std::string s;
        for (size_t i = o; i < o + n && i < d.size() && d[i] != 0; i++) {
            s.push_back((char)d[i]);
        }
        return s;
    }

    std::string first15(const std::string& s) {
        return s.substr(0, 15);
    }

    bool starts_with(const std::string& s, const std::string& prefix) {
        return s.compare(0, prefix.size(), prefix) == 0;
    }

    // Python's sum() of three floats: 0 + a, then Neumaier-compensated additions of b and c (CPython 3.12+).
    double py_sum(double a, double b, double c) {
        double hi = 0.0 + a;
        double lo = 0.0;
        for (double x : { b, c }) {
            double t = hi + x;
            if (std::fabs(hi) >= std::fabs(x)) {
                lo += (hi - t) + x;
            }
            else {
                lo += (x - t) + hi;
            }
            hi = t;
        }
        if (lo != 0.0 && std::isfinite(lo)) {
            return hi + lo;
        }
        return hi;
    }

    // Python slice length of a 4-entry list cut at n (negative n counts from the end).
    int slice4(int n) {
        return n >= 0 ? std::min(n, 4) : std::max(0, 4 + n);
    }

    // 2049 chunk directory: word 0 is the offset of {tag, offset, size or count} entries, ended by a tag that isn't
    // four letters or by the end of the data.
    using Chunks = std::map<std::string, std::pair<uint32_t, uint32_t>>;

    Chunks chunks49(const Bytes& d) {
        Chunks out;
        size_t o = u32(d, 0);
        while (o + 12 <= d.size()) {
            bool alpha = true;
            for (int i = 0; i < 4; i++) {
                uint8_t c = d[o + i];
                alpha &= (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
            }
            if (!alpha) {
                break;
            }
            out[std::string((const char*)&d[o], 4)] = { u32(d, o + 4), u32(d, o + 8) };
            o += 12;
        }
        return out;
    }

    // ------------------------------------------------------------------------------------------------------------
    // Display lists (model.py)

    constexpr uint32_t tlut_rgba16[2] = { 0xE3001001, 0x00008000 }; // G_SETOTHERMODE_H(TEXTLUT = RGBA16)
    // G_SETBLENDCOLOR as Rush 2's frame setup leaves it (0x80020178). Its alpha is the alpha-compare threshold of the
    // HUD's translucent widgets (the minimap and the radar box); 2049 lists set 0xBE, which hides them.
    constexpr uint32_t blend_rush2[2] = { 0xF9000000, 0x00000010 };
    constexpr uint16_t lod_flags_kept = 0x8007; // 2049 LOD flags with the same meaning in Rush 2.

    // Load flags of the E0 01 conditional op: bit 1 one player, 2 two players, 3 not mirrored, 4 mirrored.
    uint32_t load_flags(int players, bool mirror) {
        uint32_t f = 0;
        if (players == 1) f |= 2;
        if (players == 2) f |= 4;
        f |= mirror ? 0x10 : 0x08;
        return f;
    }

    // Ops the load-time relocator (func_8007786C / 2049 func_80096734) skips entirely.
    bool reloc_skipped(uint8_t op) {
        return (op & 0xC0) == 0x40 || (op & 0xC0) == 0x80 || (op >= 9 && op < 0x40) || (op >= 0xC0 && op < 0xD6);
    }

    // Ops whose pointer word the relocator rebases: VTX, LOAD_UCODE, DL, MTX, MOVEMEM, SETxIMG, and RDPHALF_1 when
    // followed by BRANCH_Z or LOAD_UCODE.
    bool reloc_rebased(const Bytes& d, size_t o, uint8_t op) {
        if (op == 0x01 || op == 0xDD || op == 0xDE || op == 0xDA || op == 0xDC || op >= 0xFD) {
            return true;
        }
        if (op == 0xE1 && o + 8 < d.size()) {
            return d[o + 8] == 0x04 || d[o + 8] == 0xDD;
        }
        return false;
    }

    struct RelocWalk {
        std::vector<size_t> ptrs;  // Offsets of the pointer words the relocator rebases.
        std::vector<size_t> conds; // Offsets of E0 01 conditional ops (their target word is at +4).
    };

    // Walks one model display list the way the relocator does: G_DL isn't followed, BRANCH_Z jumps to the
    // RDPHALF_1 address, G_ENDDL ends the walk.
    RelocWalk walk_reloc(const Bytes& d, size_t start) {
        RelocWalk w;
        size_t o = start;
        std::optional<uint32_t> half1;
        for (int steps = 1;; steps++) {
            if (steps > 200000 || o + 8 > d.size()) {
                fail("display list at " + hex(start) + " runs off the data (at " + hex(o) + ")");
            }
            uint8_t op = d[o];
            if (reloc_skipped(op)) {
                o += 8;
                continue;
            }
            if (reloc_rebased(d, o, op)) {
                w.ptrs.push_back(o + 4);
            }
            if (op == 0xE0 && d[o + 1] == 0x01) {
                w.conds.push_back(o);
            }
            if (op == 0xE1) {
                half1 = u32(d, o + 4) & 0xFFFFFF;
            }
            if (op == 0x04) {
                if (!half1) {
                    fail("BRANCH_Z without RDPHALF_1 at " + hex(o));
                }
                o = *half1;
                continue;
            }
            if (op == 0xDF) {
                return w;
            }
            o += 8;
        }
    }

    // Offset of the G_ENDDL ending a contiguous display list.
    size_t list_end(const Bytes& d, size_t start) {
        size_t o = start;
        while (u8(d, o) != 0xDF) {
            if (d[o] == 0x04) {
                fail("BRANCH_Z in list " + hex(start));
            }
            o += 8;
        }
        return o;
    }

    // State a list leaves set that Rush 2 relies on staying put: the texture LUT mode (othermode-H bits at shift 14)
    // and the blend colour. Follows G_DL calls and resolves conditional ops for the given load flags; a field is
    // empty if the list never sets it.
    struct EndState {
        std::optional<uint32_t> tlut;
        std::optional<uint32_t> blend;
    };

    EndState end_state(const Bytes& d, size_t start, uint32_t flags) {
        std::vector<size_t> stack;
        size_t o = start;
        EndState state;
        for (int n = 0; n < 200000; n++) {
            uint32_t w0 = u32(d, o);
            uint32_t w1 = u32(d, o + 4);
            uint32_t op = w0 >> 24;
            if (op == 0xE3 && 32 - (int)((w0 >> 8) & 0xFF) - (int)((w0 & 0xFF) + 1) == 14) {
                state.tlut = w1;
            }
            if (op == 0xF9) {
                state.blend = w1;
            }
            if (op == 0xE0 && d[o + 1] == 1) {
                uint32_t bit = u16(d, o + 2);
                if (!(bit < 32 && ((flags >> bit) & 1))) {
                    o = w1 & 0xFFFFFF;
                    continue;
                }
            }
            if (op == 0xDE) {
                if (!((w0 >> 16) & 0xFF)) {
                    stack.push_back(o + 8);
                }
                o = w1 & 0xFFFFFF;
                continue;
            }
            if (op == 0xDF) {
                if (stack.empty()) {
                    return state;
                }
                o = stack.back();
                stack.pop_back();
                continue;
            }
            o += 8;
        }
        fail("list " + hex(start) + " does not end");
    }

    // ------------------------------------------------------------------------------------------------------------
    // Rush 2049 model container (model.M49)

    struct Lod49 {
        uint16_t texture, flags;
        uint32_t distance; // f32 bits, copied as they are.
        uint32_t dl, vertices;
    };

    struct Object49 {
        std::string name;
        uint32_t radius; // f32 bits.
        uint16_t kind;
        int16_t lod_count;
        Lod49 lods[4];
    };

    struct Texture49 {
        std::string name;
        uint16_t w, h;
        uint32_t index, texels, format;
    };

    struct Palette49 {
        std::string name;
        uint32_t mode, data;
    };

    struct Model49 {
        const Bytes* d;
        Chunks c;
        uint32_t imag;
        std::vector<Object49> objects;
        std::vector<Texture49> textures;
        std::vector<Palette49> palettes;
    };

    Model49 parse_model49(const Bytes& d) {
        Model49 m;
        m.d = &d;
        m.c = chunks49(d);
        for (const char* tag : { "IMAG", "TXLD", "OBHD", "PLHD", "TXHD", "OBJS" }) {
            if (!m.c.contains(tag)) {
                fail(std::string("missing chunk ") + tag);
            }
        }
        m.imag = m.c["IMAG"].first;
        auto [obhd, obhd_count] = m.c["OBHD"];
        for (uint32_t i = 0; i < obhd_count; i++) {
            size_t r = obhd + (size_t)i * 0x58;
            Object49 ob;
            // 2049 compares 15 characters of the name; bytes after the NUL are stale.
            ob.name = cstr(d, r, 16);
            ob.radius = u32(d, r + 16);
            ob.kind = u16(d, r + 20);
            ob.lod_count = s16(d, r + 22);
            for (int j = 0; j < 4; j++) {
                size_t l = r + 0x18 + j * 16;
                ob.lods[j] = { u16(d, l), u16(d, l + 2), u32(d, l + 4), u32(d, l + 8), u32(d, l + 12) };
            }
            m.objects.push_back(ob);
        }
        auto [txhd, txhd_count] = m.c["TXHD"];
        for (uint32_t i = 0; i < txhd_count; i++) {
            size_t r = txhd + (size_t)i * 0x24;
            need(d, r + 16, 0x14);
            m.textures.push_back({ cstr(d, r, 16), u16(d, r + 16), u16(d, r + 18), u32(d, r + 20), u32(d, r + 24),
                                   u32(d, r + 28) });
        }
        auto [plhd, plhd_count] = m.c["PLHD"];
        for (uint32_t i = 0; i < plhd_count; i++) {
            size_t r = plhd + (size_t)i * 0x18;
            m.palettes.push_back({ cstr(d, r, 16), u32(d, r + 16), u32(d, r + 20) });
        }
        return m;
    }

    // OBHD names of a 2049 model file, or none if it has no chunk directory (placement.r49_model_names).
    std::set<std::string> model49_names(const Bytes& d) {
        std::set<std::string> out;
        Chunks c;
        try {
            c = chunks49(d);
        }
        catch (const ConvertError&) {
            return out;
        }
        auto it = c.find("OBHD");
        if (it == c.end()) {
            return out;
        }
        for (uint32_t k = 0; k < it->second.second; k++) {
            out.insert(cstr(d, it->second.first + (size_t)k * 0x58, 16));
        }
        return out;
    }

    // A 2049 model container holding only the objects named `keep` from `d`, with just the data they reach: their
    // lists and vertices, and the texture-load lists those call with the texels and palettes they load. Used to take
    // a few models from a big shared file.
    Bytes subset_model49(const Bytes& d, const std::set<std::string>& keep) {
        Model49 m = parse_model49(d);
        auto [txld, txld_size] = m.c["TXLD"];
        auto [objs, objs_size] = m.c["OBJS"];
        auto in_txld = [&](size_t a) { return a >= txld && a < (size_t)txld + txld_size; };
        auto in_objs = [&](size_t a) { return a >= objs && a < (size_t)objs + objs_size; };
        // Old [start, end) ranges kept, per output chunk.
        std::map<size_t, size_t> imag_r, txld_r, objs_r;
        auto add = [](std::map<size_t, size_t>& r, size_t s, size_t e) {
            auto it = r.find(s);
            if (it == r.end() || it->second < e) r[s] = e;
        };
        // Texels and palettes a texture-load list loads: each G_SETTIMG to the end of the load that follows it.
        auto add_loads = [&](size_t list) {
            size_t e = list_end(d, list);
            for (size_t o = list; o < e; o += 8) {
                if (d[o] != 0xFD) continue;
                uint32_t w0 = u32(d, o);
                size_t a = (u32(d, o + 4) & 0xFFFFFF) + m.imag;
                double bpp = std::array<double, 4>{ 0.5, 1, 2, 4 }[(w0 >> 19) & 3];
                size_t bytes = 8;
                for (size_t p = o + 8; p < e && d[p] != 0xFD; p += 8) {
                    uint32_t c1 = u32(d, p + 4);
                    if (d[p] == 0xF3) bytes = std::max(bytes, (size_t)((((c1 >> 12) & 0xFFF) + 1) * bpp));
                    if (d[p] == 0xF4) bytes = std::max(bytes, (size_t)(((c1 & 0xFFF) / 4 + 1) * ((w0 & 0xFFF) + 1) * bpp));
                    if (d[p] == 0xF0) bytes = std::max(bytes, (size_t)((((c1 >> 14) & 0x3FF) + 1) * 2));
                }
                add(imag_r, a, (a + bytes + 7) & ~(size_t)7);
            }
            add(txld_r, list, e + 8);
        };
        std::vector<const Object49*> kept;
        for (const Object49& ob : m.objects) {
            if (!keep.contains(ob.name)) continue;
            kept.push_back(&ob);
            for (int j = 0; j < slice4(ob.lod_count); j++) {
                const Lod49& l = ob.lods[j];
                if (!l.dl) continue;
                size_t e = list_end(d, l.dl);
                add(objs_r, l.dl, e + 8);
                size_t lo = l.vertices ? l.vertices : SIZE_MAX, hi = 0;
                for (size_t o = l.dl; o < e; o += 8) {
                    size_t a = u32(d, o + 4) & 0xFFFFFF;
                    if (d[o] == 0x01) {
                        lo = std::min(lo, a);
                        hi = std::max(hi, a + ((u32(d, o) >> 12) & 0xFF) * 16);
                    }
                    if (d[o] == 0xDE && in_txld(a)) add_loads(a);
                    if (d[o] == 0xDE && in_objs(a)) add(objs_r, a, list_end(d, a) + 8);
                }
                if (hi > lo) add(objs_r, lo, hi);
            }
        }

        // Layout: 8-byte header, IMAG, TXLD, OBHD, PLHD, TXHD, OBJS, chunk directory.
        Bytes out(8, 0);
        struct Seg {
            size_t start, end, to;
        };
        std::vector<Seg> segs;
        auto place = [&](const std::map<size_t, size_t>& r) {
            size_t chunk = out.size();
            size_t s = 0, e = 0;
            auto flush = [&]() {
                if (e > s) {
                    need(d, s, e - s);
                    segs.push_back({ s, e, out.size() });
                    out.insert(out.end(), d.begin() + s, d.begin() + e);
                    align8(out);
                }
            };
            for (auto [a, b] : r) { // merge overlapping ranges
                if (a <= e && e > s) { e = std::max(e, b); continue; }
                flush();
                s = a;
                e = b;
            }
            flush();
            std::pair<size_t, size_t> placed{ chunk, out.size() - chunk };
            // A gap after each chunk: merge_models maps a chunk's end address into that chunk, so the next chunk must
            // not start there.
            out.resize(out.size() + 8, 0);
            return placed;
        };
        auto map = [&](size_t a) -> size_t {
            for (const Seg& s : segs) {
                if (a >= s.start && a < s.end) return a - s.start + s.to;
            }
            fail("subset: " + hex(a) + " not kept");
        };
        auto imag = place(imag_r);
        auto txld_new = place(txld_r);
        size_t new_imag = imag.first;
        size_t obhd = out.size();
        out.resize(obhd + kept.size() * 0x58, 0);
        size_t plhd = out.size();
        // No texture or palette records: the kept lists call their texture loads directly, and an extra record would
        // change the texture table Rush 2's car code walks.
        size_t txhd = out.size();
        auto objs_new = place(objs_r);
        // Pointers: model lists hold file offsets, texture-load lists IMAG-relative ones.
        for (const Object49* ob : kept) {
            for (int j = 0; j < slice4(ob->lod_count); j++) {
                if (!ob->lods[j].dl) continue;
                std::vector<size_t> lists = { ob->lods[j].dl };
                for (size_t o = ob->lods[j].dl; o < list_end(d, ob->lods[j].dl); o += 8) {
                    if (d[o] == 0xDE && in_objs(u32(d, o + 4) & 0xFFFFFF)) lists.push_back(u32(d, o + 4) & 0xFFFFFF);
                }
                for (size_t l : lists) {
                    RelocWalk w = walk_reloc(d, l);
                    std::vector<size_t> words = w.ptrs;
                    for (size_t c : w.conds) words.push_back(c + 4);
                    for (size_t p : words) {
                        uint32_t v = u32(d, p);
                        put32(out, map(p), (v & 0xFF000000) | (uint32_t)map(v & 0xFFFFFF));
                    }
                }
            }
        }
        for (auto [s, e] : txld_r) {
            for (size_t o = s; o + 8 <= e; o += 8) {
                if (d[o] == 0xFD || (d[o] == 0xE1 && o + 8 < e && d[o + 8] == 0x04)) {
                    uint32_t v = u32(d, o + 4);
                    put32(out, map(o + 4), (v & 0xFF000000) | (uint32_t)(map((v & 0xFFFFFF) + m.imag) - new_imag));
                }
            }
        }
        for (size_t k = 0; k < kept.size(); k++) {
            size_t r = obhd + k * 0x58;
            size_t rec = m.c["OBHD"].first + (size_t)(kept[k] - m.objects.data()) * 0x58;
            std::copy(d.begin() + rec, d.begin() + rec + 0x58, out.begin() + r);
            for (int j = 0; j < 4; j++) {
                const Lod49& l = kept[k]->lods[j];
                put32(out, r + 0x18 + j * 16 + 8, l.dl && j < slice4(kept[k]->lod_count) ? (uint32_t)map(l.dl) : 0);
                put32(out, r + 0x18 + j * 16 + 12, l.vertices && j < slice4(kept[k]->lod_count) ? (uint32_t)map(l.vertices) : 0);
            }
        }
        size_t dir = out.size();
        auto chunk = [&](const char* tag, size_t o, size_t n) {
            out.insert(out.end(), tag, tag + 4);
            add32(out, (uint32_t)o);
            add32(out, (uint32_t)n);
        };
        chunk("IMAG", imag.first, imag.second);
        chunk("TXLD", txld_new.first, txld_new.second);
        chunk("OBHD", obhd, kept.size());
        chunk("PLHD", plhd, 0);
        chunk("TXHD", txhd, 0);
        chunk("OBJS", objs_new.first, objs_new.second);
        add32(out, 0);
        put32(out, 0, (uint32_t)dir);
        return out;
    }

    // ------------------------------------------------------------------------------------------------------------
    // Geometry: 2049 model containers merged into one Rush 2 container (track.merge_models)

    struct MergeOptions {
        std::map<std::string, std::string> rename;
        std::vector<std::string> dummies;         // Extra empty models.
        std::vector<std::string> dummy_textures;  // Extra textures whose load list is an empty list.
        const std::set<std::string>* exclude;     // Models left out (their data stays, unreferenced).
        std::multimap<std::string, std::string> aliases; // 2049 object -> extra names drawing the same lists.
        // Model (output name) -> 2049 object whose first list each of the model's lists also calls at its end.
        std::multimap<std::string, std::string> appends;
        std::map<std::string, uint16_t> kinds;    // Model (output name) -> behaviour id (default: the 2049 object's).
    };

    Bytes merge_models(const std::vector<const Bytes*>& files, const MergeOptions& opt, std::set<std::string>& names) {
        constexpr size_t header_size = 0x28;
        std::vector<Model49> srcs;
        for (const Bytes* f : files) {
            srcs.push_back(parse_model49(*f));
        }
        Bytes out(header_size, 0);

        // Old [start, end) of each moved chunk of each file and its new start.
        struct Move {
            size_t start, end, to;
        };
        std::vector<std::vector<Move>> moved(srcs.size());
        auto place = [&](const char* kind) {
            for (size_t i = 0; i < srcs.size(); i++) {
                auto [o, n] = srcs[i].c[kind];
                align8(out);
                need(*srcs[i].d, o, n);
                moved[i].push_back({ o, (size_t)o + n, out.size() });
                out.insert(out.end(), srcs[i].d->begin() + o, srcs[i].d->begin() + o + n);
            }
        };
        place("IMAG");
        align8(out);
        size_t txld_start = out.size();
        place("TXLD");
        size_t txld_end = out.size();
        place("OBJS");

        auto new_addr = [&](size_t i, size_t a) -> size_t {
            for (const Move& m : moved[i]) {
                if ((m.start <= a && a < m.end) || (a == m.end && m.start < m.end)) {
                    return a - m.start + m.to;
                }
            }
            fail("file " + std::to_string(i) + ": pointer " + hex(a) + " outside IMAG/TXLD/OBJS");
        };

        // Rewrites the pointer word at old offset o_old of file i (in a moved chunk) to the new address of its target.
        std::unordered_set<size_t> done;
        auto fix = [&](size_t i, size_t o_old, bool add_imag) {
            size_t o_new = new_addr(i, o_old);
            if (!done.insert(o_new).second) {
                return;
            }
            uint32_t w = u32(*srcs[i].d, o_old);
            size_t a = (w & 0xFFFFFF) + (add_imag ? srcs[i].imag : 0);
            put32(out, o_new, (w & 0xFF000000) | (uint32_t)new_addr(i, a));
        };

        std::map<std::pair<size_t, uint32_t>, size_t> lod_lists; // (file, old list) -> new list
        for (size_t i = 0; i < srcs.size(); i++) {
            const Model49& m = srcs[i];
            for (const Object49& ob : m.objects) {
                for (int j = 0; j < slice4(ob.lod_count); j++) {
                    uint32_t dl = ob.lods[j].dl;
                    if (!dl || lod_lists.contains({ i, dl })) {
                        continue;
                    }
                    RelocWalk w = walk_reloc(*m.d, dl);
                    for (size_t p : w.ptrs) {
                        fix(i, p, false);
                    }
                    for (size_t c : w.conds) {
                        fix(i, c + 4, false);
                    }
                    lod_lists[{ i, dl }] = new_addr(i, dl);
                }
            }
            // Texture-load lists: SETTIMG and RDPHALF_1 before BRANCH_Z are IMAG-relative.
            auto [ta, ts] = m.c.at("TXLD");
            const Bytes& d = *m.d;
            for (size_t o = ta; o < (size_t)ta + ts; o += 8) {
                uint8_t op = u8(d, o);
                if (op == 0xFD || (op == 0xE1 && o + 8 < (size_t)ta + ts && u8(d, o + 8) == 0x04)) {
                    fix(i, o + 4, true);
                }
            }
        }

        // Lists that leave the texture LUT mode or the blend colour other than Rush 2 expects get a copy ending in a
        // restore of both.
        std::vector<size_t> starts;
        for (auto& [key, s] : lod_lists) {
            starts.push_back(s);
        }
        std::sort(starts.begin(), starts.end());
        std::map<size_t, size_t> restored;
        for (size_t s : starts) {
            bool leaks = false;
            for (bool mirror : { false, true }) {
                EndState state = end_state(out, s, load_flags(1, mirror));
                leaks |= (state.tlut && *state.tlut != tlut_rgba16[1]) || (state.blend && *state.blend != blend_rush2[1]);
            }
            if (!leaks) {
                continue;
            }
            size_t e = list_end(out, s);
            size_t ns = out.size();
            RelocWalk w = walk_reloc(out, s);
            Bytes body(out.begin() + s, out.begin() + e);
            std::vector<size_t> words = w.ptrs;
            for (size_t c : w.conds) {
                words.push_back(c + 4);
            }
            for (size_t p : words) {
                uint32_t v = u32(out, p);
                size_t target = v & 0xFFFFFF;
                if (s <= target && target <= e) {
                    put32(body, p - s, (v & 0xFF000000) | (uint32_t)(target - s + ns));
                }
            }
            out.insert(out.end(), body.begin(), body.end());
            add32(out, tlut_rgba16[0]);
            add32(out, tlut_rgba16[1]);
            add32(out, blend_rush2[0]);
            add32(out, blend_rush2[1]);
            add32(out, 0xDF000000);
            add32(out, 0);
            restored[s] = ns;
        }
        size_t empty_dl = out.size();
        add32(out, 0xDF000000);
        add32(out, 0);

        // Model names are unique across the files (the first wins) and sorted for Rush 2's binary search.
        struct Entry {
            std::string name;
            size_t file;
            const Object49* ob; // Null for a dummy.
            bool alias = false; // Draws another entry's lists, through copies of them.
        };
        std::vector<Entry> entries;
        std::set<std::string> have;
        for (size_t i = 0; i < srcs.size(); i++) {
            for (const Object49& ob : srcs[i].objects) {
                auto r = opt.rename.find(ob.name);
                std::string n = first15(r != opt.rename.end() ? r->second : ob.name);
                if (have.contains(n) || (opt.exclude && opt.exclude->contains(n))) {
                    continue;
                }
                have.insert(n);
                entries.push_back({ n, i, &ob });
                auto [lo, hi] = opt.aliases.equal_range(ob.name);
                for (auto it = lo; it != hi; ++it) {
                    std::string alias = first15(it->second);
                    if (have.insert(alias).second) {
                        entries.push_back({ alias, i, &ob, true });
                    }
                }
            }
        }
        for (const std::string& dummy : opt.dummies) {
            std::string n = first15(dummy);
            if (have.insert(n).second) {
                entries.push_back({ n, 0, nullptr });
            }
        }
        std::stable_sort(entries.begin(), entries.end(), [](const Entry& a, const Entry& b) { return a.name < b.name; });

        // Rush 2's loader rebases the pointers of each model's lists (func_8007786C), so a list two models share would
        // be rebased twice: an alias gets its own copy of each list (the vertices and texture loads stay shared).
        auto copy_list = [&](size_t s) {
            size_t e = list_end(out, s);
            size_t ns = out.size();
            RelocWalk w = walk_reloc(out, s);
            Bytes body(out.begin() + s, out.begin() + e);
            std::vector<size_t> words = w.ptrs;
            for (size_t c : w.conds) {
                words.push_back(c + 4);
            }
            for (size_t p : words) {
                if (p < s || p >= e) {
                    continue;
                }
                uint32_t v = u32(out, p);
                size_t target = v & 0xFFFFFF;
                if (s <= target && target <= e) {
                    put32(body, p - s, (v & 0xFF000000) | (uint32_t)(target - s + ns));
                }
            }
            out.insert(out.end(), body.begin(), body.end());
            add32(out, 0xDF000000);
            add32(out, 0);
            return ns;
        };
        // The relocated (and restored) list of a source object's LOD.
        auto final_list = [&](size_t file, uint32_t dl) -> size_t {
            auto it = dl ? lod_lists.find({ file, dl }) : lod_lists.end();
            if (it == lod_lists.end()) {
                return 0;
            }
            auto rs = restored.find(it->second);
            return rs != restored.end() ? rs->second : it->second;
        };
        // A copy of list s that calls `calls` before its end, then sets s's first render mode and combiner again.
        auto extend_list = [&](size_t s, const std::vector<size_t>& calls) {
            size_t ns = copy_list(s);
            out.resize(out.size() - 8); // the copy's G_ENDDL
            for (size_t c : calls) {
                add32(out, 0xDE000000);
                add32(out, (uint32_t)c);
            }
            for (uint8_t op : { 0xE2, 0xFC }) {
                for (size_t o = s; o < list_end(out, s); o += 8) {
                    if (out[o] == op) {
                        add32(out, u32(out, o));
                        add32(out, u32(out, o + 4));
                        break;
                    }
                }
            }
            add32(out, 0xDF000000);
            add32(out, 0);
            return ns;
        };
        std::map<std::pair<std::string, int>, size_t> alias_lists;
        for (const Entry& e : entries) {
            if (!e.alias) {
                continue;
            }
            for (int j = 0; j < std::min(4, (int)e.ob->lod_count); j++) {
                uint32_t dl = e.ob->lods[j].dl;
                auto it = dl ? lod_lists.find({ e.file, dl }) : lod_lists.end();
                if (it == lod_lists.end()) {
                    continue;
                }
                size_t ndl = it->second;
                auto rs = restored.find(ndl);
                alias_lists[{ e.name, j }] = copy_list(rs != restored.end() ? rs->second : ndl);
            }
        }
        std::map<std::pair<std::string, int>, size_t> append_lists;
        for (const Entry& e : entries) {
            auto [lo, hi] = opt.appends.equal_range(e.name);
            if (e.ob == nullptr || lo == hi) {
                continue;
            }
            std::vector<size_t> calls;
            for (auto it = lo; it != hi; ++it) {
                for (size_t i = 0; i < srcs.size(); i++) {
                    for (const Object49& ob : srcs[i].objects) {
                        size_t l = ob.name == it->second && ob.lod_count > 0 ? final_list(i, ob.lods[0].dl) : 0;
                        if (l != 0) {
                            calls.push_back(l);
                        }
                    }
                }
            }
            for (int j = 0; j < std::min(4, (int)e.ob->lod_count); j++) {
                auto al = alias_lists.find({ e.name, j });
                size_t s = al != alias_lists.end() ? al->second : final_list(e.file, e.ob->lods[j].dl);
                if (s != 0 && !calls.empty()) {
                    append_lists[{ e.name, j }] = extend_list(s, calls);
                }
            }
        }

        size_t model_off = out.size();
        for (const Entry& e : entries) {
            Bytes rec;
            if (e.ob == nullptr) {
                add32(rec, 1);
                add16(rec, 0);
                add16(rec, 0);
                add_float(rec, 0.0);
                add32(rec, (uint32_t)empty_dl);
            }
            else {
                add32(rec, (uint32_t)std::max(1, std::min(4, (int)e.ob->lod_count)));
                for (int j = 0; j < std::min(4, (int)e.ob->lod_count); j++) {
                    const Lod49& l = e.ob->lods[j];
                    size_t ndl = 0;
                    if (l.dl) {
                        auto it = lod_lists.find({ e.file, l.dl });
                        if (it == lod_lists.end()) {
                            fail("list of " + e.name + " not relocated");
                        }
                        ndl = it->second;
                        auto rs = restored.find(ndl);
                        if (rs != restored.end()) {
                            ndl = rs->second;
                        }
                        auto al = alias_lists.find({ e.name, j });
                        if (al != alias_lists.end()) {
                            ndl = al->second;
                        }
                        auto ap = append_lists.find({ e.name, j });
                        if (ap != append_lists.end()) {
                            ndl = ap->second;
                        }
                    }
                    add16(rec, l.texture);
                    add16(rec, l.flags & lod_flags_kept);
                    add32(rec, l.distance);
                    add32(rec, (uint32_t)ndl);
                }
            }
            rec.resize(0x34, 0);
            out.insert(out.end(), rec.begin(), rec.end());
        }

        // A merged file's palette indices are offset by the palettes of the files before it.
        struct Palette {
            std::string name;
            uint32_t mode;
            size_t data;
        };
        std::vector<Palette> pals;
        std::vector<size_t> pal_base;
        for (size_t i = 0; i < srcs.size(); i++) {
            pal_base.push_back(pals.size());
            for (const Palette49& p : srcs[i].palettes) {
                pals.push_back({ p.name, p.mode, new_addr(i, (size_t)p.data + srcs[i].imag) });
            }
        }
        struct Texture {
            std::string name;
            uint16_t w, h;
            uint32_t index;
            size_t data;
            uint32_t format;
        };
        std::vector<Texture> textures;
        std::set<std::string> tex_names;
        for (size_t i = 0; i < srcs.size(); i++) {
            for (const Texture49& t : srcs[i].textures) {
                std::string n = first15(t.name);
                if (!tex_names.insert(n).second) {
                    continue;
                }
                uint32_t idx = t.index;
                if ((idx & 0xFFFF) != 0xFFFF) {
                    idx = (idx & 0xFFFF0000) | ((idx + (uint32_t)pal_base[i]) & 0xFFFF);
                }
                textures.push_back({ n, t.w, t.h, idx, new_addr(i, (size_t)t.texels + srcs[i].imag), t.format });
            }
        }
        for (const std::string& dummy : opt.dummy_textures) {
            std::string n = first15(dummy);
            if (tex_names.insert(n).second) {
                textures.push_back({ n, 0, 0, 0xFFFF, empty_dl, 0 });
            }
        }
        size_t tex_off = out.size();
        for (const Texture& t : textures) {
            add_name(out, t.name);
            add16(out, t.w);
            add16(out, t.h);
            add32(out, t.index);
            add32(out, (uint32_t)t.data);
            add32(out, t.format);
        }
        size_t pal_off = out.size();
        for (const Palette& p : pals) {
            add_name(out, p.name);
            add32(out, p.mode);
            add32(out, (uint32_t)p.data);
        }
        size_t name_off = out.size();
        for (const Entry& e : entries) {
            add_name(out, e.name);
            if (e.ob != nullptr) {
                add32(out, e.ob->radius);
                auto kind = opt.kinds.find(e.name);
                add16(out, kind != opt.kinds.end() ? kind->second : e.ob->kind);
                add16(out, 0);
            }
            else {
                add32(out, 0);
                add32(out, 0);
            }
            names.insert(e.name);
        }
        const size_t header[10] = { model_off, name_off, tex_off, pal_off, entries.size(), textures.size(), pals.size(),
                                    txld_start, txld_end, 0 };
        for (int i = 0; i < 10; i++) {
            put32(out, i * 4, (uint32_t)header[i]);
        }
        return out;
    }

    // ------------------------------------------------------------------------------------------------------------
    // Placement (placement.convert_ex)

    // Rush 2049's dynamic-object types (main data 0x80117530, 122 x 0x30): name prefix, model, kind and sub-kind.
    struct Type49 {
        std::string name;
        std::string model; // Empty = none.
        uint8_t kind, sub;
    };

    constexpr uint32_t main_vram = 0x80086A50;

    std::string main_string(const Bytes& main, uint32_t vram) {
        size_t o = (size_t)(vram - main_vram);
        if (vram < main_vram || o >= main.size()) {
            fail("string address " + hex(vram) + " outside 2049 main data");
        }
        size_t e = o;
        while (e < main.size() && main[e] != 0) {
            e++;
        }
        if (e == main.size()) {
            fail("unterminated string at " + hex(vram));
        }
        return std::string((const char*)&main[o], e - o);
    }

    std::vector<Type49> read_types(const Bytes& main) {
        std::vector<Type49> types;
        for (uint32_t k = 0; k < 122; k++) {
            size_t o = 0x80117530 + k * 0x30 - main_vram;
            Type49 t;
            t.name = main_string(main, u32(main, o));
            uint32_t model = u32(main, o + 4);
            if (model) {
                t.model = main_string(main, model);
            }
            uint32_t w5 = u32(main, o + 20);
            t.kind = (w5 >> 8) & 0xFF;
            t.sub = w5 & 0xFF;
            types.push_back(t);
        }
        return types;
    }

    // func_800ABCC8: the first type whose name is a prefix of the object name.
    const Type49* classify(const std::vector<Type49>& types, const std::string& name) {
        for (const Type49& t : types) {
            if (starts_with(name, t.name)) {
                return &t;
            }
        }
        return nullptr;
    }

    struct Record {
        int index;
        std::string name;
        double m[9];   // Row-major 3x3, row-vector convention.
        double pos[3];
        uint32_t flags;
        int next, child;
        double bbox[6];
    };

    // A child transform made relative to its parent node: local = (world - p_parent) * P^-1.
    void relative(const double* cm, const double* cpos, const Record& parent, double* m, double* pos) {
        const double* P = parent.m;
        double a = P[0], b = P[1], c = P[2];
        double d = P[3], e = P[4], f = P[5];
        double g = P[6], h = P[7], i = P[8];
        double det = a * (e * i - f * h) - b * (d * i - f * g) + c * (d * h - e * g);
        double dp[3];
        for (int k = 0; k < 3; k++) {
            dp[k] = cpos[k] - parent.pos[k];
        }
        if (std::fabs(det) < 1e-12) {
            memcpy(m, cm, sizeof(double) * 9);
            memcpy(pos, dp, sizeof(dp));
            return;
        }
        double inv[3][3] = {
            { (e * i - f * h) / det, (c * h - b * i) / det, (b * f - c * e) / det },
            { (f * g - d * i) / det, (a * i - c * g) / det, (c * d - a * f) / det },
            { (d * h - e * g) / det, (b * g - a * h) / det, (a * e - b * d) / det },
        };
        for (int col = 0; col < 3; col++) {
            pos[col] = py_sum(dp[0] * inv[0][col], dp[1] * inv[1][col], dp[2] * inv[2][col]);
        }
        for (int row = 0; row < 3; row++) {
            for (int col = 0; col < 3; col++) {
                m[row * 3 + col] = py_sum(cm[row * 3] * inv[0][col], cm[row * 3 + 1] * inv[1][col],
                                          cm[row * 3 + 2] * inv[2][col]);
            }
        }
    }

    // Path node orientation quaternion (x, y, z, w) and scale -> row-major matrix (row-vector convention).
    void quat_to_m(const double* q, const double* s, double* m) {
        double x = q[0], y = q[1], z = q[2], w = q[3];
        double n = std::sqrt(x * x + y * y + z * z + w * w);
        if (n == 0.0) {
            n = 1.0;
        }
        x = x / n;
        y = y / n;
        z = z / n;
        w = w / n;
        double r[9] = {
            1 - 2 * (y * y + z * z), 2 * (x * y + z * w), 2 * (x * z - y * w),
            2 * (x * y - z * w), 1 - 2 * (x * x + z * z), 2 * (y * z + x * w),
            2 * (x * z + y * w), 2 * (y * z - x * w), 1 - 2 * (x * x + y * y),
        };
        for (int row = 0; row < 3; row++) {
            for (int col = 0; col < 3; col++) {
                m[row * 3 + col] = r[row * 3 + col] * s[row];
            }
        }
    }

    // 2049 dynamic type -> Rush 2 placement name, resolved by Rush 2's breakable table to a model in its shared assets.
    const std::map<std::string, std::string> r49_to_r2 = {
        { "YIELDHIT", "YIELDHIT" }, { "SHATPANE", "SHATPANE" }, { "FLAG2", "FLAG2" }, { "COLLISION", "COLLISION" },
    };
    // Coins become Rush 2 key records (the KEY class, behaviour 8): silver coins KEYS0-7 and gold coins KEYG0-7,
    // numbered per kind in record order. src/collectibles.cpp gives each its bit (silver 0-7, gold 8-15) and draws
    // 2049's coin model.
    const std::map<std::string, std::string> r49_coins = { { "SILVERCOIN", "KEYS" }, { "GOLDCOIN", "KEYG" } };
    constexpr int coins_per_kind = 8;
    // Types dropped: 2049-only game systems, or nothing to show (WEPICON* by prefix, other kind 6 objects). BULB and
    // GUARDRAIL have no model and type flags 0x60004, which 2049's spawner (func_800ABCC8) refuses: they do nothing
    // in 2049 either.
    const std::set<std::string> r49_drop = {
        "BULB", "GUARDRAIL", "WPR_MINE", "TRIGGER",
    };

    // Objects 2049 knocks over when a car hits them (src/track2049_props.cpp): kind 2 (CONE1, GASPUMP, RAT, RATCONE),
    // the signs (kind 0, sub-kinds 1-2) and CACTUS.
    bool is_prop(const Type49& t) {
        return t.kind == 2 || (t.kind == 0 && (t.sub == 1 || t.sub == 2)) || t.name == "CACTUS";
    }

    // A prop's model in the converted geometry.
    std::string prop_model(const std::string& model) {
        return ("X49" + model).substr(0, 15);
    }

    Bytes convert_placement(const Bytes& data, const Bytes& geometry, const std::string& prefix,
                            const std::vector<Type49>& types, const std::set<std::string>& extra_models,
                            bool static_paths, std::vector<rush2::track2049::PathRecord>* path_records,
                            std::vector<rush2::track2049::SpinRecord>* spin_records,
                            std::vector<rush2::track2049::PropRecord>* prop_records) {
        // 2049 file: u32 directory offset, u32 chunk count; WHDR = Rush 2's header (count, {offset, name[16]}),
        // WOBJ = 0x68-byte records (Rush 2's 0x64 with a dynamic-object id at +0x4C, the box moved to +0x50).
        constexpr size_t rec49 = 0x68;
        uint32_t dir = u32(data, 0);
        uint32_t chunk_count = u32(data, 4);
        Chunks chunks;
        for (uint32_t k = 0; k < chunk_count; k++) {
            size_t o = dir + (size_t)k * 12;
            need(data, o, 12);
            chunks[std::string((const char*)&data[o], 4)] = { u32(data, o + 4), u32(data, o + 8) };
        }
        for (const char* tag : { "WHDR", "WOBJ", "GTLD", "GDAT" }) {
            if (!chunks.contains(tag)) {
                fail(std::string("placement: missing chunk ") + tag);
            }
        }
        uint32_t whdr = chunks["WHDR"].first;
        auto [obj_off, obj_count] = chunks["WOBJ"];
        int64_t root = 0;
        if (u32(data, whdr) > 0) {
            int64_t diff = (int64_t)u32(data, whdr + 4) - obj_off;
            root = diff >= 0 ? diff / (int64_t)rec49 : -((-diff + (int64_t)rec49 - 1) / (int64_t)rec49); // Floor.
        }
        std::vector<Record> recs;
        for (uint32_t i = 0; i < obj_count; i++) {
            size_t o = obj_off + (size_t)i * rec49;
            need(data, o, rec49);
            Record r;
            r.index = (int)i;
            r.name = cstr(data, o, 16);
            for (int k = 0; k < 9; k++) r.m[k] = f32(data, o + 0x10 + k * 4);
            for (int k = 0; k < 3; k++) r.pos[k] = f32(data, o + 0x34 + k * 4);
            r.flags = u32(data, o + 0x40);
            r.next = s16(data, o + 0x44);
            r.child = s16(data, o + 0x46);
            for (int k = 0; k < 6; k++) r.bbox[k] = f32(data, o + 0x50 + k * 4);
            recs.push_back(r);
        }

        // The sibling/child tree (func_80081790) must reach each record at most once.
        {
            std::vector<bool> seen(recs.size());
            std::function<void(int64_t)> link = [&](int64_t i) {
                while (i >= 0) {
                    if (i >= (int64_t)recs.size() || seen[i]) {
                        fail("placement tree loops or points outside the file (record " + std::to_string(i) + ")");
                    }
                    seen[i] = true;
                    if (recs[i].child >= 0) {
                        link(recs[i].child);
                    }
                    i = recs[i].next;
                }
            };
            if (!recs.empty()) {
                link(root);
            }
        }

        std::set<std::string> geo_names = model49_names(geometry);
        geo_names.insert(extra_models.begin(), extra_models.end());
        auto model_ok = [&](const std::string& n) { return geo_names.contains(n) || geo_names.contains(first15(n)); };

        enum class How { kept, mapped, model };
        std::map<int, std::pair<std::string, How>> keep;
        std::map<int, int> spin_sub; // Record index -> sub-kind of objects that turn in place (func_8010E694).
        std::map<int, int> prop_type; // Record index -> type row of props.
        std::map<std::string, int> coins;
        for (const Record& r : recs) {
            const Type49* t = classify(types, r.name);
            if (t == nullptr) {
                if (model_ok(r.name)) {
                    keep[r.index] = { r.name, How::kept };
                }
                continue;
            }
            auto coin = r49_coins.find(t->name);
            if (coin != r49_coins.end()) {
                int& n = coins[t->name];
                if (n < coins_per_kind) {
                    keep[r.index] = { coin->second + std::to_string(n), How::mapped };
                    n++;
                }
                continue;
            }
            if (r49_drop.contains(t->name) || starts_with(t->name, "WEPICON") || t->kind == 6) {
                continue;
            }
            if (is_prop(*t)) {
                if (!t->model.empty() && model_ok(prop_model(t->model))) {
                    keep[r.index] = { prop_model(t->model), How::model };
                    prop_type[r.index] = (int)(t - types.data());
                }
                continue;
            }
            auto mapped = r49_to_r2.find(t->name);
            if (mapped != r49_to_r2.end()) {
                std::string suffix = r.name.find("_FW") != std::string::npos ? "_F" :
                                     r.name.find("_BW") != std::string::npos ? "_B" : "";
                keep[r.index] = { mapped->second + suffix, How::mapped };
                continue;
            }
            if (!t->model.empty() && model_ok(t->model)) {
                keep[r.index] = { first15(t->model), How::model };
                if (t->kind == 0 && t->sub >= 3 && t->sub <= 5) {
                    spin_sub[r.index] = t->sub;
                }
            }
        }

        // Rebuild the tree in the original order and nesting; children of a dropped record move up to its parent.
        std::vector<std::pair<const Record*, int>> order; // (record, parent index in order or -1)
        std::function<void(int64_t, int)> walk = [&](int64_t i, int parent_out) {
            while (i >= 0) {
                const Record& r = recs[i];
                if (keep.contains(r.index)) {
                    int idx = (int)order.size();
                    order.push_back({ &r, parent_out });
                    if (r.child >= 0) {
                        walk(r.child, idx);
                    }
                }
                else if (r.child >= 0) {
                    walk(r.child, parent_out);
                }
                i = r.next;
            }
        };
        walk(root, -1);

        // Path objects (geometry PTHD chunk, 36-byte headers {name[16], u32 flags, s16 count, s16 trigger, u32 0,
        // u32 node offset, s32 id}; PATH = 68-byte nodes {f32 pos[3], dir[3], scale[3], quat[4], dist, time, speed,
        // u32 flags}) placed statically at their spawn nodes (node flag 1, else the first node).
        struct Extra {
            std::string model;
            double m[9];
            double pos[3];
            int path, node;
        };
        std::vector<Extra> extras;
        if (static_paths) {
            Chunks c = chunks49(geometry);
            auto pthd = c.find("PTHD");
            if (pthd != c.end()) {
                auto [o, n] = pthd->second;
                for (uint32_t k = 0; k < n; k++) {
                    size_t h = o + (size_t)k * 36;
                    std::string name = cstr(geometry, h, 16);
                    int count = s16(geometry, h + 20);
                    uint32_t nodes = u32(geometry, h + 28);
                    need(geometry, h, 36);
                    struct Node {
                        double pos[3], scale[3], quat[4];
                        uint32_t flags;
                    };
                    std::vector<Node> path_nodes;
                    for (int j = 0; j < count; j++) {
                        size_t p = nodes + (size_t)j * 68;
                        need(geometry, p, 68);
                        Node nd;
                        for (int a = 0; a < 3; a++) nd.pos[a] = f32(geometry, p + a * 4);
                        for (int a = 0; a < 3; a++) nd.scale[a] = f32(geometry, p + 0x18 + a * 4);
                        for (int a = 0; a < 4; a++) nd.quat[a] = f32(geometry, p + 0x24 + a * 4);
                        nd.flags = u32(geometry, p + 0x40);
                        path_nodes.push_back(nd);
                    }
                    const Type49* t = classify(types, name);
                    if (t == nullptr || t->model.empty() || !model_ok(t->model)) {
                        continue;
                    }
                    std::vector<const Node*> spawns;
                    for (const Node& nd : path_nodes) {
                        if (nd.flags & 1) {
                            spawns.push_back(&nd);
                        }
                    }
                    if (spawns.empty() && !path_nodes.empty()) {
                        spawns.push_back(&path_nodes[0]);
                    }
                    for (const Node* nd : spawns) {
                        Extra e;
                        e.model = t->model;
                        e.path = (int)k;
                        e.node = (int)(nd - path_nodes.data());
                        quat_to_m(nd->quat, nd->scale, e.m);
                        memcpy(e.pos, nd->pos, sizeof(e.pos));
                        extras.push_back(e);
                    }
                }
            }
        }

        struct Item {
            std::string name;
            double m[9], pos[3];
            uint32_t flags;
            double bbox[6];
            int parent;
            int path = -1, node = -1;
            int spin = 0;
            int prop = -1;                  // Type row of a prop.
            const Record* source = nullptr; // The 2049 record (world pose).
            const Record* parent_source = nullptr;
        };
        std::vector<Item> items;
        std::vector<int> top;
        for (size_t k = 0; k < order.size(); k++) {
            if (order[k].second == -1) {
                top.push_back((int)k);
            }
        }
        // The top-level section whose culling box holds pos (relative to the section or absolute).
        auto section_for = [&](const double* pos) {
            for (int k : top) {
                const Record& r = *order[k].first;
                const double* lo = r.bbox;
                const double* hi = r.bbox + 3;
                if (lo[0] == hi[0] && lo[1] == hi[1] && lo[2] == hi[2]) {
                    continue;
                }
                bool rel = true, abs = true;
                for (int a = 0; a < 3; a++) {
                    double v = pos[a] - r.pos[a];
                    rel &= lo[a] <= v && v <= hi[a];
                    abs &= lo[a] <= pos[a] && pos[a] <= hi[a];
                }
                if (rel || abs) {
                    return k;
                }
            }
            return top.empty() ? -1 : top[0];
        };

        for (auto& [r, par] : order) {
            auto& [name, how] = keep[r->index];
            Item it;
            it.name = name;
            memcpy(it.m, r->m, sizeof(it.m));
            memcpy(it.pos, r->pos, sizeof(it.pos));
            // Rush 2 makes only breakables parent-relative by itself.
            if (par >= 0 && how != How::mapped) {
                relative(r->m, r->pos, *order[par].first, it.m, it.pos);
            }
            // Node bits 0x380000 / 0xFFC00000 hold Rush 2's culling-box type and index (func_8007FC80).
            it.flags = r->flags & 0x7FFFF;
            for (int a = 0; a < 6; a++) {
                it.bbox[a] = par == -1 ? r->bbox[a] : 0.0;
            }
            it.parent = par;
            auto spin = spin_sub.find(r->index);
            it.spin = spin == spin_sub.end() ? 0 : spin->second;
            auto prop = prop_type.find(r->index);
            it.prop = prop == prop_type.end() ? -1 : prop->second;
            it.source = r;
            it.parent_source = par >= 0 ? order[par].first : nullptr;
            items.push_back(it);
        }
        // Path objects are world-space top-level records after the sections, like Rush 2049's unparented objects:
        // the visibility tables only hide the first sections, so these stay drawn wherever the object moves.
        for (const Extra& e : extras) {
            Item it;
            it.name = first15(e.model);
            memcpy(it.m, e.m, sizeof(it.m));
            memcpy(it.pos, e.pos, sizeof(it.pos));
            it.flags = 0x40;
            for (double& b : it.bbox) {
                b = 0.0;
            }
            it.parent = -1;
            it.path = e.path;
            it.node = e.node;
            items.push_back(it);
        }

        // Children lists in item order; the file is written as a pre-order walk, like the stock files.
        std::map<int, std::vector<int>> kids;
        for (size_t k = 0; k < items.size(); k++) {
            kids[items[k].parent].push_back((int)k);
        }
        std::vector<int> new_order;
        std::function<void(int)> emit = [&](int par) {
            auto it = kids.find(par);
            if (it == kids.end()) {
                return;
            }
            for (int k : it->second) {
                new_order.push_back(k);
                emit(k);
            }
        };
        emit(-1);
        std::vector<int> pos_of(items.size(), -1);
        for (size_t n = 0; n < new_order.size(); n++) {
            pos_of[new_order[n]] = (int)n;
        }
        std::vector<int> next(items.size(), -1), child(items.size(), -1);
        for (auto& [par, lst] : kids) {
            for (size_t a = 0; a + 1 < lst.size(); a++) {
                next[lst[a]] = pos_of[lst[a + 1]];
            }
            if (par >= 0) {
                child[par] = pos_of[lst[0]];
            }
        }
        if (new_order.size() > 0x7FFF) {
            fail("placement: too many records");
        }
        if (path_records != nullptr) {
            for (size_t n = 0; n < new_order.size(); n++) {
                const Item& it = items[new_order[n]];
                if (it.path >= 0) {
                    path_records->push_back({ it.path, it.node, (int)n });
                }
                if (it.spin != 0 && spin_records != nullptr) {
                    spin_records->push_back({ (int)n, it.spin });
                }
            }
        }
        if (prop_records != nullptr) {
            for (size_t n = 0; n < new_order.size(); n++) {
                const Item& it = items[new_order[n]];
                if (it.prop < 0) {
                    continue;
                }
                rush2::track2049::PropRecord p{};
                p.record = (int)n;
                p.type = it.prop;
                const std::string& name = it.source->name;
                p.direction = name.find("_FW") != std::string::npos ? 1 : name.find("_BW") != std::string::npos ? 2 : 0;
                for (int a = 0; a < 9; a++) {
                    p.m[a] = (float)it.source->m[a];
                    p.parent_m[a] = it.parent_source != nullptr ? (float)it.parent_source->m[a] : (a % 4 == 0 ? 1.0f : 0.0f);
                }
                for (int a = 0; a < 3; a++) {
                    p.pos[a] = (float)it.source->pos[a];
                    p.parent_pos[a] = it.parent_source != nullptr ? (float)it.parent_source->pos[a] : 0.0f;
                }
                prop_records->push_back(p);
            }
        }

        // Rush 2 file: u32 1, {u32 offset 0x18, name[16]}, then 0x64-byte records {name[16], f32 m[9], f32 pos[3],
        // u32 flags, s16 next, s16 child, u32 0, f32 bbmin[3], bbmax[3]}.
        Bytes out;
        add32(out, 1);
        add32(out, 0x18);
        add_name(out, prefix);
        for (int k : new_order) {
            const Item& it = items[k];
            add_name(out, it.name);
            for (double v : it.m) add_float(out, v);
            for (double v : it.pos) add_float(out, v);
            add32(out, it.flags);
            add16(out, (uint16_t)next[k]);
            add16(out, (uint16_t)child[k]);
            add32(out, 0);
            for (double v : it.bbox) add_float(out, v);
        }
        return out;
    }

    // ------------------------------------------------------------------------------------------------------------
    // Collision (collision.convert)

    constexpr size_t seg_size = 0x84, node_size = 0x14, poly_size = 0x18, vert_size = 8, mover_size = 0x20;
    constexpr uint16_t spline_flags = 0x3000;

    struct CNode {
        int parent;
        uint8_t mask;          // Bit q: child q is a node, else a leaf. 2049: bit 4+q adds 0x10000 to the leaf offset.
        int16_t bounds[4];
        uint32_t child[4];     // Node index, or the raw leaf offset.
        uint32_t leaf[4];      // Leaf key (offset) of leaf quadrants, 0 = empty.
        bool dead;
    };

    struct CPoly {
        uint16_t flags, info;
        int16_t matrix[9];
        uint16_t voff;
        std::vector<int> verts;
        int seg; // -1 = none.
        size_t vend;
    };

    struct CLeaf {
        std::vector<int> polys;
        int raw; // Interned encoded bytes (Collision::raws).
    };

    struct Collision {
        std::vector<const uint8_t*> segs;
        std::vector<CNode> nodes;
        std::vector<CPoly> polys;
        std::vector<const uint8_t*> verts;
        Bytes vlist, leaf_blob;
        std::map<uint32_t, CLeaf> leaves; // By offset (or merge key).
        std::vector<Bytes> raws;
        std::map<Bytes, int> raw_ids;

        // Referenced-leaf bookkeeping for the leaf section size: references per leaf key, referenced keys per raw.
        std::unordered_map<uint32_t, int> leaf_refs;
        std::vector<int> raw_refs;
        size_t section_size = 1;

        int intern(const Bytes& raw) {
            auto [it, added] = raw_ids.try_emplace(raw, (int)raws.size());
            if (added) {
                raws.push_back(raw);
                raw_refs.push_back(0);
            }
            return it->second;
        }

        void ref(uint32_t key, int delta) {
            if (!key) {
                return;
            }
            int& n = leaf_refs[key];
            int before = n;
            n += delta;
            if ((before == 0) == (n == 0)) {
                return;
            }
            int raw = leaves.at(key).raw;
            int& r = raw_refs[raw];
            if (n > 0) {
                if (r++ == 0) section_size += raws[raw].size();
            }
            else if (--r == 0) {
                section_size -= raws[raw].size();
            }
        }

        void ref_node(const CNode& n, int delta) {
            for (int q = 0; q < 4; q++) {
                if (!(n.mask & (1 << q))) {
                    ref(n.leaf[q], delta);
                }
            }
        }
    };

    // One polygon's run-length vertex list: u16 start, then if more remain and the next byte >= mark, (byte & mask)
    // more consecutive indices.
    std::vector<int> decode_vlist(const Bytes& buf, size_t& off, int count, uint8_t mark, uint8_t mask) {
        std::vector<int> out;
        int left = count;
        while (left > 0) {
            int v = u16(buf, off);
            off += 2;
            int run = 0;
            if (left >= 2 && u8(buf, off) >= mark) {
                run = buf[off] & mask;
                off += 1;
            }
            left -= run + 1;
            for (int i = 0; i <= run; i++) {
                out.push_back(v + i);
            }
        }
        return out;
    }

    void encode_vlist(const std::vector<int>& idx, uint8_t mark, uint8_t mask, Bytes& out) {
        size_t i = 0;
        while (i < idx.size()) {
            int v = idx[i];
            if (v >= (mark << 8)) {
                fail("collision: vertex index " + hex(v) + " collides with the run marker");
            }
            size_t j = i + 1;
            while (j < idx.size() && idx[j] == idx[j - 1] + 1 && j - i <= mask) {
                j++;
            }
            add16(out, v);
            if (j - i > 1) {
                out.push_back((uint8_t)(mark | (j - i - 1)));
            }
            i = j;
        }
    }

    // Leaf list: u8 count, then u16 entries {bits 15-13 run length - 1, bits 12-0 first polygon}.
    std::vector<int> decode_leaf(const Bytes& buf, size_t& off) {
        int count = u8(buf, off);
        off += 1;
        std::vector<int> out;
        int left = count;
        while (left > 0) {
            int v = u16(buf, off);
            off += 2;
            int run = v >> 13;
            for (int i = 0; i <= run; i++) {
                out.push_back((v & 0x1FFF) + i);
            }
            left -= run + 1;
        }
        return out;
    }

    // Ascending polygon indices as a leaf list (runs of up to 8).
    Bytes encode_leaf(const std::vector<int>& polys) {
        Bytes out{ (uint8_t)polys.size() };
        size_t i = 0;
        while (i < polys.size()) {
            size_t j = i + 1;
            while (j < polys.size() && polys[j] == polys[j - 1] + 1 && j - i < 8) {
                j++;
            }
            add16(out, (uint32_t)(((j - i - 1) << 13) | polys[i]));
            i = j;
        }
        return out;
    }

    // Parses a 2049 file: header {u16 nSeg, nNode, nPoly, nVert, nMover, vlistBytes; u32 leafBytes}, then SEG NODE
    // POLY VERT MOVER VLIST LEAF.
    void parse_collision49(const Bytes& data, Collision& c) {
        size_t n_seg = u16(data, 0), n_node = u16(data, 2), n_poly = u16(data, 4), n_vert = u16(data, 6);
        size_t n_mover = u16(data, 8), vlist_bytes = u16(data, 10), leaf_bytes = u32(data, 12);
        size_t o = 16;
        size_t expect = o + n_seg * seg_size + n_node * node_size + n_poly * poly_size + n_vert * vert_size +
                        n_mover * mover_size + vlist_bytes + leaf_bytes;
        if (expect != data.size()) {
            fail("collision size " + hex(data.size()) + " != header total " + hex(expect));
        }
        for (size_t i = 0; i < n_seg; i++, o += seg_size) {
            c.segs.push_back(&data[o]);
        }
        for (size_t i = 0; i < n_node; i++, o += node_size) {
            CNode n{};
            n.parent = s16(data, o);
            n.mask = data[o + 3];
            for (int k = 0; k < 4; k++) n.bounds[k] = s16(data, o + 4 + k * 2);
            for (int k = 0; k < 4; k++) n.child[k] = u16(data, o + 12 + k * 2);
            c.nodes.push_back(n);
        }
        for (size_t i = 0; i < n_poly; i++, o += poly_size) {
            CPoly p{};
            p.flags = u16(data, o);
            p.info = u16(data, o + 2);
            for (int k = 0; k < 9; k++) p.matrix[k] = s16(data, o + 4 + k * 2);
            p.voff = u16(data, o + 22);
            c.polys.push_back(p);
        }
        for (size_t i = 0; i < n_vert; i++, o += vert_size) {
            c.verts.push_back(&data[o]);
        }
        // MOVER: u16 object id, u16 poly, s16 rest matrix[9], u16 vertex, u8 rest vertex[8]. Dropped: the moving
        // polygons stay at their rest pose.
        for (size_t i = 0; i < n_mover; i++, o += mover_size) {
            if (u16(data, o + 2) >= n_poly) {
                fail("collision: mover " + std::to_string(i) + " polygon out of range");
            }
            if (u16(data, o + 22) >= n_vert) {
                fail("collision: mover " + std::to_string(i) + " vertex out of range");
            }
        }
        c.vlist.assign(data.begin() + o, data.begin() + o + vlist_bytes);
        o += vlist_bytes;
        c.leaf_blob.assign(data.begin() + o, data.begin() + o + leaf_bytes);

        for (CPoly& p : c.polys) {
            p.seg = -1;
            if (p.voff >= c.vlist.size()) {
                fail("collision: polygon vertex list offset out of range");
            }
            size_t end = p.voff;
            p.verts = decode_vlist(c.vlist, end, p.info & 0xF, 0xE0, 0x1F);
            if ((p.flags & spline_flags) && end + 2 <= c.vlist.size()) {
                p.seg = (c.vlist[end] << 8) | c.vlist[end + 1];
                end += 2;
            }
            p.vend = end;
        }
        for (CNode& n : c.nodes) {
            for (int q = 0; q < 4; q++) {
                if (n.mask & (1 << q)) {
                    continue;
                }
                uint32_t off = n.child[q] | (((n.mask >> 4) & (1 << q)) ? 0x10000 : 0);
                n.leaf[q] = off;
                if (off && !c.leaves.contains(off) && off < c.leaf_blob.size()) {
                    size_t end = off;
                    CLeaf leaf;
                    leaf.polys = decode_leaf(c.leaf_blob, end);
                    leaf.raw = c.intern(Bytes(c.leaf_blob.begin() + off, c.leaf_blob.begin() + end));
                    c.leaves[off] = std::move(leaf);
                }
            }
        }
    }

    // Collision::validate for a 2049 file: what would make the converted file unusable.
    void validate_collision49(const Collision& c) {
        size_t n_seg = c.segs.size(), n_node = c.nodes.size(), n_poly = c.polys.size(), n_vert = c.verts.size();
        for (size_t i = 0; i < n_node; i++) {
            const CNode& n = c.nodes[i];
            std::string at = "collision: node " + std::to_string(i);
            if (i == 0 && n.parent != -1) {
                fail(at + " has a parent");
            }
            if (i && !(n.parent >= 0 && (size_t)n.parent < n_node)) {
                fail(at + " parent out of range");
            }
            if (n.bounds[0] > n.bounds[1] || n.bounds[2] > n.bounds[3]) {
                fail(at + " bad bounds");
            }
            for (int q = 0; q < 4; q++) {
                if (n.mask & (1 << q)) {
                    uint32_t k = n.child[q];
                    if (k == 0) {
                        continue; // Null child.
                    }
                    if (!(k < n_node)) {
                        fail(at + " child out of range");
                    }
                    if (c.nodes[k].parent != (int)i) {
                        fail(at + " child parent mismatch");
                    }
                }
                else if (n.leaf[q] && n.leaf[q] >= c.leaf_blob.size()) {
                    fail(at + " leaf offset past the leaf section");
                }
            }
        }
        for (auto& [off, leaf] : c.leaves) {
            if (leaf.polys.size() != c.raws[leaf.raw][0]) {
                fail("collision: leaf " + hex(off) + " count mismatch");
            }
            for (int p : leaf.polys) {
                if ((size_t)p >= n_poly) {
                    fail("collision: leaf " + hex(off) + " polygon out of range");
                }
            }
        }
        std::vector<std::pair<size_t, size_t>> ends;
        for (size_t i = 0; i < n_poly; i++) {
            const CPoly& p = c.polys[i];
            std::string at = "collision: polygon " + std::to_string(i);
            size_t n = p.info & 0xF;
            if (n < 3) {
                fail(at + " has fewer than 3 vertices");
            }
            if (p.verts.size() != n) {
                fail(at + " vertex list length mismatch");
            }
            for (int v : p.verts) {
                if ((size_t)v >= n_vert) {
                    fail(at + " vertex out of range");
                }
                if (v >= 0xE000) {
                    fail(at + " vertex collides with the run marker");
                }
            }
            if ((p.flags & spline_flags) && (p.seg < 0 || (size_t)p.seg >= n_seg)) {
                fail(at + " spline segment out of range");
            }
            ends.push_back({ p.voff, p.vend });
        }
        std::sort(ends.begin(), ends.end());
        for (size_t i = 0; i + 1 < ends.size(); i++) {
            if (ends[i].second > ends[i + 1].first) {
                fail("collision: vertex lists overlap at " + hex(ends[i].first));
            }
        }
        if (!ends.empty() && ends.back().second > c.vlist.size()) {
            fail("collision: last vertex list runs past the section");
        }
    }

    // 2049 POLY+2 word -> Rush 2's: bits 14/15 (Rush 2's tunnel level) come only from 2049's covered bit 8; bits 4/5
    // (2049 conveyor/platform) and their parameter in bits 11-15 are dropped.
    uint16_t convert_info(uint16_t info) {
        uint16_t v = (info & 0x30) ? (info & 0x7FF) : info;
        uint16_t out = v & 0x3EFF & ~0x30;
        if (v & 0x100) {
            out |= 0x4000;
        }
        return out;
    }

    // Collapses bottom-level quadtree nodes into one leaf in their parent until the leaf section fits 16-bit offsets
    // (Collision.merge_leaves). The merged leaf holds the union of its children's polygons; the ground and wall
    // queries test every listed polygon exactly, so the result is the same. Largest saving first, as a heap of
    // (-saving, node).
    void merge_leaves(Collision& c, size_t limit) {
        uint32_t next_key = (c.leaves.empty() ? 0 : c.leaves.rbegin()->first) + 0x1000000;

        struct Candidate {
            int64_t saved;
            std::vector<int> polys;
            Bytes raw;
        };
        auto candidate = [&](int i) -> std::optional<Candidate> {
            const CNode& n = c.nodes[i];
            if (i == 0 || n.dead) {
                return std::nullopt;
            }
            for (int q = 0; q < 4; q++) {
                if ((n.mask & (1 << q)) && n.child[q] != 0) {
                    return std::nullopt;
                }
            }
            std::set<int> polys;
            int64_t saved = 0;
            for (int q = 0; q < 4; q++) {
                if (!(n.mask & (1 << q)) && n.leaf[q]) {
                    const CLeaf& leaf = c.leaves.at(n.leaf[q]);
                    polys.insert(leaf.polys.begin(), leaf.polys.end());
                    saved += (int64_t)c.raws[leaf.raw].size();
                }
            }
            if (polys.size() > 255) {
                return std::nullopt;
            }
            Candidate cand;
            cand.polys.assign(polys.begin(), polys.end());
            cand.raw = encode_leaf(cand.polys);
            cand.saved = saved - (int64_t)cand.raw.size();
            return cand;
        };

        using Entry = std::pair<int64_t, int>;
        std::priority_queue<Entry, std::vector<Entry>, std::greater<Entry>> heap;
        for (size_t i = 0; i < c.nodes.size(); i++) {
            if (auto cand = candidate((int)i)) {
                heap.push({ -cand->saved, (int)i });
            }
        }
        while (!heap.empty() && c.section_size > limit) {
            int i = heap.top().second;
            heap.pop();
            auto cand = candidate(i);
            if (!cand) {
                continue;
            }
            CNode& n = c.nodes[i];
            CNode& par = c.nodes[n.parent];
            uint32_t key = next_key++;
            c.leaves[key] = { cand->polys, c.intern(cand->raw) };
            for (int k = 0; k < 4; k++) {
                if ((par.mask & (1 << k)) && par.child[k] == (uint32_t)i) {
                    par.mask &= ~(1 << k);
                    par.leaf[k] = key;
                    par.child[k] = 0;
                    c.ref(key, 1);
                }
            }
            c.ref_node(n, -1);
            n.dead = true;
            if (auto pc = candidate(n.parent)) {
                heap.push({ -pc->saved, n.parent });
            }
        }

        // Compact the node indices, keeping the source order.
        std::vector<int> new_index(c.nodes.size(), -1);
        std::vector<CNode> nodes;
        for (size_t i = 0; i < c.nodes.size(); i++) {
            if (!c.nodes[i].dead) {
                new_index[i] = (int)nodes.size();
                nodes.push_back(c.nodes[i]);
            }
        }
        for (CNode& n : nodes) {
            if (n.parent >= 0) {
                n.parent = new_index[n.parent];
                if (n.parent < 0) {
                    fail("collision: merged node still has children");
                }
            }
            for (int k = 0; k < 4; k++) {
                if ((n.mask & (1 << k)) && n.child[k]) {
                    if (new_index[n.child[k]] < 0) {
                        fail("collision: merged node still referenced");
                    }
                    n.child[k] = new_index[n.child[k]];
                }
            }
        }
        c.nodes = std::move(nodes);
        if (c.section_size > limit) {
            fail("collision: leaf section still " + hex(c.section_size) + " after merging");
        }
    }

    // Rush 2 file: header {u16 nSeg, nNode, nPoly, nVert, leafBytes, vlistBytes}, then SEG NODE POLY VERT LEAF VLIST.
    // Leaf lists are deduplicated (byte 0 stays reserved so offset 0 means empty); vertex lists keep the source order
    // with Rush 2's run markers (0xC0 | n).
    Bytes build_collision_rush2(const Collision& c) {
        std::vector<size_t> order(c.polys.size());
        for (size_t i = 0; i < order.size(); i++) {
            order[i] = i;
        }
        std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) { return c.polys[a].voff < c.polys[b].voff; });
        Bytes vblob;
        std::vector<size_t> voffs(c.polys.size());
        for (size_t i : order) {
            const CPoly& p = c.polys[i];
            voffs[i] = vblob.size();
            encode_vlist(p.verts, 0xC0, 0x3F, vblob);
            if (p.flags & spline_flags) {
                add16(vblob, p.seg);
            }
        }
        if (vblob.size() > 0xFFFF) {
            fail("collision: vertex-list section " + hex(vblob.size()) + " exceeds 16 bits");
        }

        std::set<uint32_t> used;
        for (const CNode& n : c.nodes) {
            for (int q = 0; q < 4; q++) {
                if (!(n.mask & (1 << q)) && n.leaf[q]) {
                    used.insert(n.leaf[q]);
                }
            }
        }
        Bytes lblob{ 0 };
        std::map<int, size_t> placed; // raw -> offset
        std::map<uint32_t, size_t> remap;
        for (auto& [off, leaf] : c.leaves) {
            if (!used.contains(off)) {
                continue;
            }
            auto [it, added] = placed.try_emplace(leaf.raw, lblob.size());
            if (added) {
                const Bytes& raw = c.raws[leaf.raw];
                lblob.insert(lblob.end(), raw.begin(), raw.end());
            }
            remap[off] = it->second;
        }
        if (lblob.size() > 0xFFFF) {
            fail("collision: leaf section " + hex(lblob.size()) + " exceeds 16-bit leaf offsets");
        }
        for (size_t count : { c.segs.size(), c.nodes.size(), c.polys.size(), c.verts.size() }) {
            if (count > 0xFFFF) {
                fail("collision: too many records");
            }
        }

        Bytes out;
        for (size_t v : { c.segs.size(), c.nodes.size(), c.polys.size(), c.verts.size(), lblob.size(), vblob.size() }) {
            add16(out, (uint32_t)v);
        }
        for (const uint8_t* s : c.segs) {
            out.insert(out.end(), s, s + seg_size);
        }
        for (const CNode& n : c.nodes) {
            add16(out, (uint16_t)n.parent);
            out.push_back(0);
            out.push_back(n.mask & 0x0F);
            for (int16_t b : n.bounds) {
                add16(out, (uint16_t)b);
            }
            for (int q = 0; q < 4; q++) {
                uint32_t v = n.child[q];
                if (!(n.mask & (1 << q))) {
                    v = n.leaf[q] ? (uint32_t)remap.at(n.leaf[q]) : 0;
                }
                if (v > 0xFFFF) {
                    fail("collision: node child out of range");
                }
                add16(out, v);
            }
        }
        for (size_t i = 0; i < c.polys.size(); i++) {
            const CPoly& p = c.polys[i];
            add16(out, p.flags);
            add16(out, p.info);
            for (int16_t m : p.matrix) {
                add16(out, (uint16_t)m);
            }
            add16(out, (uint32_t)voffs[i]);
        }
        for (const uint8_t* v : c.verts) {
            out.insert(out.end(), v, v + vert_size);
        }
        out.insert(out.end(), lblob.begin(), lblob.end());
        out.insert(out.end(), vblob.begin(), vblob.end());
        return out;
    }

    Bytes convert_collision(const Bytes& data) {
        Collision c;
        parse_collision49(data, c);
        validate_collision49(c);
        for (CPoly& p : c.polys) {
            p.info = convert_info(p.info);
            if ((p.flags & 0xF) == 0xF) {
                fail("collision: source polygon already disabled (type 0xF)");
            }
        }
        for (const CNode& n : c.nodes) {
            c.ref_node(n, 1);
        }
        if (c.section_size > 0xFFFF) {
            merge_leaves(c, 0xFFFF);
        }
        return build_collision_rush2(c);
    }

    // ------------------------------------------------------------------------------------------------------------
    // AI paths (paths.convert): the same format and loader in both games, so the file is kept after the structural
    // checks of paths.validate. (Its recomputed load-time indices are always in range once these pass.)
    //   header 12 bytes {u16 base time, s16 loop/finish/arm checkpoint, s16 checkpoint count, s16 pad}, 10 x 0x50
    //   checkpoints {f32 pos[3], f32 dir[3], ...}, route header at 0x32C {u16 spine count, ..., u8 branch count at
    //   +8}, 16-byte branches {u8, s8 from, u16 from index, s8 to, u8, u16 to index, s8 checkpoint, u8, u16 count,
    //   u32}, u16 point total, spine and branch points (6 bytes each), 4 lanes {u16 count, u8, u8, u32} + count x 8.

    void validate_path(const Bytes& d) {
        constexpr size_t route = 0x32C;
        int n_cp = s16(d, 8);
        std::vector<std::array<double, 3>> dirs;
        for (int i = 0; i < n_cp; i++) {
            size_t o = 0xC + (size_t)i * 0x50;
            need(d, o, 0x50);
            dirs.push_back({ f32(d, o + 0xC), f32(d, o + 0x10), f32(d, o + 0x14) });
        }
        size_t n_spine = u16(d, route);
        size_t n_branch = u8(d, route + 8);
        size_t o = route + 16;
        struct Branch {
            int from_path, from_idx, to_path, to_idx, cp, count;
        };
        std::vector<Branch> branches;
        for (size_t i = 0; i < n_branch; i++, o += 16) {
            need(d, o, 16);
            branches.push_back({ (int8_t)d[o + 1], u16(d, o + 2), (int8_t)d[o + 4], u16(d, o + 6), (int8_t)d[o + 8],
                                 u16(d, o + 10) });
        }
        size_t n_total = u16(d, o);
        o += 2 + 6 * n_spine;
        for (const Branch& b : branches) {
            o += 6 * (size_t)b.count;
        }
        need(d, 0, o);
        int lane_counts[4];
        for (int& count : lane_counts) {
            count = u16(d, o);
            o += 8 + 8 * (size_t)count;
            need(d, 0, o);
        }

        if (o != d.size()) {
            fail("path: parsed " + hex(o) + " of " + hex(d.size()));
        }
        if (n_cp < 1 || n_cp > 10) {
            fail("path: checkpoint count " + std::to_string(n_cp) + " not in 1..10");
        }
        if (n_branch > 15) {
            fail("path: more than 15 branches");
        }
        size_t total = n_spine;
        for (const Branch& b : branches) {
            total += b.count;
        }
        if (total != n_total) {
            fail("path: point total mismatch");
        }
        if (n_spine < 2) {
            fail("path: spine too short");
        }
        for (size_t bi = 0; bi < branches.size(); bi++) {
            const Branch& b = branches[bi];
            for (auto [path, idx] : { std::pair{ b.from_path, b.from_idx }, std::pair{ b.to_path, b.to_idx } }) {
                if (path >= (int)branches.size() || path < -1) {
                    fail("path: branch " + std::to_string(bi) + " path out of range");
                }
                int lim = path < 0 ? (int)n_spine : branches[path].count;
                if (!(idx >= 0 && idx < lim)) {
                    fail("path: branch " + std::to_string(bi) + " index out of range");
                }
            }
            if (!(b.cp >= -1 && b.cp < n_cp)) {
                fail("path: branch " + std::to_string(bi) + " checkpoint out of range");
            }
            if (b.count < 2) {
                fail("path: branch " + std::to_string(bi) + " too short");
            }
        }
        for (int count : lane_counts) {
            if (count < 2) {
                fail("path: lane too short");
            }
        }
        for (size_t i = 0; i < dirs.size(); i++) {
            double n = std::sqrt(dirs[i][0] * dirs[i][0] + dirs[i][1] * dirs[i][1] + dirs[i][2] * dirs[i][2]);
            if (std::fabs(n - 1) > 0.02) {
                fail("path: checkpoint " + std::to_string(i) + " direction not unit length");
            }
        }
    }

    // The floors of a Rush 2049 collision file over and under points (paths.floor_heights): each upward-facing,
    // non-wall polygon as a fan of triangles, with its x/z bounds. A polygon's vertex 0 is its world origin and the
    // rest are in its local frame, whose axes are the rows of its matrix (row 1 is the normal).
    struct FloorTriangle {
        double v[3][3];
        double x0, x1, z0, z1;
    };

    std::vector<FloorTriangle> floor_triangles(const Bytes& collision_2049) {
        constexpr double min_normal_y = 0.25;
        Collision c;
        parse_collision49(collision_2049, c);
        auto vertex = [&](int k, double* out) {
            const uint8_t* r = c.verts.at((size_t)k);
            int16_t x = (int16_t)((r[0] << 8) | r[1]), y = (int16_t)((r[2] << 8) | r[3]), z = (int16_t)((r[4] << 8) | r[5]);
            uint16_t f = (uint16_t)((r[6] << 8) | r[7]);
            out[0] = (x * 32 + ((f >> 10) & 31)) / 32.0;
            out[1] = (y * 32 + ((f >> 5) & 31)) / 32.0;
            out[2] = (z * 32 + (f & 31)) / 32.0;
        };
        std::vector<FloorTriangle> tris;
        for (const CPoly& p : c.polys) {
            int type = p.flags & 0xF;
            if (p.verts.size() < 3 || type == 5 || type == 6 || p.matrix[4] / 16384.0 < min_normal_y) {
                continue;
            }
            std::vector<std::array<double, 3>> w(p.verts.size());
            vertex(p.verts[0], w[0].data());
            for (size_t i = 1; i < p.verts.size(); i++) {
                double l[3];
                vertex(p.verts[i], l);
                for (int a = 0; a < 3; a++) {
                    w[i][a] = w[0][a];
                    for (int r = 0; r < 3; r++) {
                        w[i][a] += l[r] * p.matrix[3 * r + a] / 16384.0;
                    }
                }
            }
            for (size_t i = 1; i + 1 < w.size(); i++) {
                FloorTriangle t;
                for (int a = 0; a < 3; a++) {
                    t.v[0][a] = w[0][a];
                    t.v[1][a] = w[i][a];
                    t.v[2][a] = w[i + 1][a];
                }
                t.x0 = std::min({ t.v[0][0], t.v[1][0], t.v[2][0] });
                t.x1 = std::max({ t.v[0][0], t.v[1][0], t.v[2][0] });
                t.z0 = std::min({ t.v[0][2], t.v[1][2], t.v[2][2] });
                t.z1 = std::max({ t.v[0][2], t.v[1][2], t.v[2][2] });
                tris.push_back(t);
            }
        }
        return tris;
    }

    std::vector<double> floor_heights(const std::vector<FloorTriangle>& tris, double x, double z) {
        std::vector<double> out;
        for (const FloorTriangle& t : tris) {
            if (x < t.x0 || x > t.x1 || z < t.z0 || z > t.z1) {
                continue;
            }
            double xa = t.v[0][0], ya = t.v[0][1], za = t.v[0][2];
            double xb = t.v[1][0], yb = t.v[1][1], zb = t.v[1][2];
            double xc = t.v[2][0], yc = t.v[2][1], zc = t.v[2][2];
            double d = (zb - zc) * (xa - xc) + (xc - xb) * (za - zc);
            if (d == 0) {
                continue;
            }
            double u = ((zb - zc) * (x - xc) + (xc - xb) * (z - zc)) / d;
            double v = ((zc - za) * (x - xc) + (xa - xc) * (z - zc)) / d;
            double w = 1 - u - v;
            if (u < -1e-6 || v < -1e-6 || w < -1e-6) {
                continue;
            }
            out.push_back(u * ya + v * yb + w * yc);
        }
        return out;
    }

    // A stunt arena's path (paths.spine_lanes). Rush 2 starts a stunt race at spine point 0, facing point 1, at the
    // spine's height (func_800A34A8), and puts a crashed car back on the nearest point of its lanes (func_80090A40).
    // The arenas' spines lie on their floors already; as a safeguard each spine point is put on the highest floor
    // under it (up to floor_slack above), or failing that on the lowest floor at most floor_reach above it, rounded
    // up. The spine is rotated to start at the first point that, with its successor,
    // already lay on the floor (within floor_slack), or failing that has floor under it (the spine is a closed loop on
    // every arena).
    // Rush 2049 runs no AI on its arenas, and their four lanes are stubs of 3-4 points whose load-time crossings
    // (func_80092D6C) all land on the last point; func_8006DB00 then steps a lane from its last point to that same
    // point, and Rush 2's lane follower (func_80074990) never gets past it. Each lane is replaced with the spine, with
    // the stubs' speed 100 and flags 2. A validated file has no branches and its lanes last.
    Bytes spine_lanes(const Bytes& d, const Bytes& collision_2049) {
        constexpr size_t route = 0x32C;
        constexpr double floor_slack = 2, floor_reach = 64;
        size_t n_spine = u16(d, route);
        if (u8(d, route + 8) != 0) {
            fail("path: stunt arena path has branches");
        }
        size_t spine = route + 16 + 2;
        std::vector<FloorTriangle> tris = floor_triangles(collision_2049);
        std::vector<std::array<int16_t, 3>> pts;
        std::vector<int> grounded;
        for (size_t i = 0; i < n_spine; i++) {
            int x = s16(d, spine + 6 * i), y = s16(d, spine + 6 * i + 2), z = s16(d, spine + 6 * i + 4);
            std::vector<double> hs = floor_heights(tris, x, z);
            std::optional<double> below, above;
            for (double h : hs) {
                if (h <= y + floor_slack) {
                    if (!below || h > *below) below = h;
                }
                else if (h <= y + floor_reach) {
                    if (!above || h < *above) above = h;
                }
            }
            int on = 0;
            if (below || above) {
                double floor = below ? *below : *above;
                on = y - floor_slack <= floor ? 2 : 1;
                y = (int)std::clamp(std::ceil(floor), -32768.0, 32767.0);
            }
            pts.push_back({ (int16_t)x, (int16_t)y, (int16_t)z });
            grounded.push_back(on);
        }
        size_t first = 0;
        bool found = false;
        for (int q : { 2, 1 }) {
            for (size_t i = 0; i < n_spine && !found; i++) {
                if (std::min(grounded[i], grounded[(i + 1) % n_spine]) >= q) {
                    first = i;
                    found = true;
                }
            }
        }
        std::rotate(pts.begin(), pts.begin() + (ptrdiff_t)first, pts.end());
        Bytes out(d.begin(), d.begin() + (ptrdiff_t)spine);
        for (const auto& p : pts) {
            for (int16_t v : p) add16(out, (uint16_t)v);
        }
        for (int lane = 0; lane < 4; lane++) {
            add16(out, (uint32_t)n_spine);
            add16(out, 0);
            add32(out, 0);
            for (const auto& p : pts) {
                for (int16_t v : p) add16(out, (uint16_t)v);
                out.push_back(100);
                out.push_back(2);
            }
        }
        return out;
    }

    // ------------------------------------------------------------------------------------------------------------
    // Tables in 2049's main data

    constexpr uint32_t main_rom = 0xB0CB10;
    constexpr uint32_t fog_colours_vram = 0x80114658; // 3 bytes per 2049 track id.
    constexpr uint32_t pvs_counts_vram = 0x8011E748;  // u8 per 2049 track id.
    constexpr uint32_t demo_lists_vram = 0x801173D8;  // ptr per race track + 6 * backward: demo start spine indices.
    constexpr uint32_t demo_counts_vram = 0x80117408; // s16 per race track + 6 * backward.
    constexpr uint32_t pvs_vram[6] = { 0x8011B898, 0x8011BFE8, 0x8011C738, 0x8011CE88, 0x8011D618, 0x8011DC88 };
    constexpr int shared_model_file = 78; // F1FLAG / F2FLAG frames, TRIGGEROFF / TRIGGERON.
    constexpr int coin_model_file = 68;   // GOLDCOIN / SILVERCOIN; their models run Rush 2's key behaviour (8).
    const std::map<std::string, uint16_t> coin_kinds = { { "GOLDCOING_COIN", 8 }, { "SILVERCOINS_COI", 8 } };

    // 2049 PVS entry: four u32, bit (i & 31) of word i >> 5 = section i visible. Rush 2 entry: two u64, sections
    // 0-63 then 64-127. So the words go out in the order 1, 0, 3, 2.
    Bytes convert_pvs(const Bytes& main, int k, int count) {
        Bytes out;
        for (int reg = 0; reg < count; reg++) {
            size_t o = pvs_vram[k - 1] - main_vram + (size_t)reg * 16;
            for (int j : { 1, 0, 3, 2 }) {
                add32(out, u32(main, o + j * 4));
            }
        }
        return out;
    }

    // ------------------------------------------------------------------------------------------------------------
    // Animated textures (docs/rush2049_research/texanim.md). Both tables are indexed by 2049's track id (race track
    // k = id k - 1) and hold 0x14-byte records. Textures are looked up by name in the converted geometry, as 2049
    // looks them up in its loaded model files (func_800B24EC); a record whose textures aren't there is left out, as
    // are the FIREGEN flip-books (fire effect textures that no race track has) and palette cycles (kinds 0-8, which
    // no race track uses).

    constexpr uint32_t flip_lists_vram = 0x8011A31C;   // Flip-books: s16 count (0 ends), s16 start, s16 forward,
                                                       // s16 current, f32 timer, f32 period, frame table pointer.
    constexpr uint32_t scroll_lists_vram = 0x8011A840; // Scrolls: name (0 ends), s16 position, s16 wrap, s8 speed,
                                                       // u8 kind, s16 rate, f32 timer, data pointer.
    constexpr uint32_t texture_direct = 0x08000000;    // Texture record flag: data points at texels, not a load list.

    struct TextureRecord {
        uint32_t data = 0;  // Load list (or texels) offset.
        uint32_t flags = 0;
    };

    // The converted geometry's texture record named `name` (15 characters compared, like the lookups).
    bool find_texture(const Bytes& g, const std::string& name, TextureRecord& out) {
        uint32_t table = u32(g, 8), count = u32(g, 20);
        for (uint32_t i = 0; i < count; i++) {
            size_t r = table + (size_t)i * 0x20;
            if (first15(cstr(g, r, 16)) == first15(name)) {
                out.data = u32(g, r + 0x18);
                out.flags = u32(g, r + 0x1C);
                return true;
            }
        }
        return false;
    }

    // Offsets of the commands with opcode `op` in the texture-load list at `list`, up to its G_ENDDL (the walk of
    // func_800BD080 / func_800BD104). Empty if the list isn't inside the texture-load list range.
    std::vector<uint32_t> load_list_commands(const Bytes& g, uint32_t list, uint8_t op) {
        std::vector<uint32_t> out;
        uint32_t start = u32(g, 28), end = u32(g, 32);
        if (list < start || list >= end || (list & 7) != 0) {
            return out;
        }
        for (uint32_t o = list; o < end; o += 8) {
            if (g[o] == op) {
                out.push_back(o);
            }
            if (g[o] == 0xDF) {
                return out;
            }
        }
        return {};
    }

    rush2::track2049::TexAnims convert_tex_anims(const Bytes& main, int k, const Bytes& g) {
        rush2::track2049::TexAnims out;
        uint32_t list = u32(main, flip_lists_vram - main_vram + (size_t)(k - 1) * 4);
        for (size_t o = list - main_vram; list != 0 && s16(main, o) != 0; o += 0x14) {
            int count = s16(main, o);
            rush2::track2049::TexFlipbook f;
            f.start = s16(main, o + 2);
            f.forward = s16(main, o + 4) != 0;
            uint32_t period = u32(main, o + 0xC);
            memcpy(&f.period, &period, 4);
            uint32_t frames = u32(main, o + 0x10);
            bool ok = count > 0 && f.start >= 0 && f.start < count;
            for (int i = 0; ok && i < count; i++) {
                TextureRecord t;
                std::string name = main_string(main, u32(main, frames - main_vram + (size_t)i * 12));
                ok = find_texture(g, name, t);
                if (!ok) {
                    break;
                }
                if (i == f.start) {
                    std::vector<uint32_t> settimg = load_list_commands(g, t.data, 0xFD);
                    ok = !(t.flags & texture_direct) && !settimg.empty();
                    f.target = name;
                    f.settimg = ok ? settimg[0] : 0;
                }
                if (t.flags & texture_direct) {
                    f.frames.push_back(t.data);
                } else {
                    std::vector<uint32_t> settimg = load_list_commands(g, t.data, 0xFD);
                    ok = ok && !settimg.empty();
                    f.frames.push_back(ok ? u32(g, settimg[0] + 4) : 0);
                }
            }
            if (ok) {
                out.flipbooks.push_back(std::move(f));
            }
        }
        list = u32(main, scroll_lists_vram - main_vram + (size_t)(k - 1) * 4);
        for (size_t o = list - main_vram; list != 0 && u32(main, o) != 0; o += 0x14) {
            uint8_t kind = u8(main, o + 9);
            TextureRecord t;
            rush2::track2049::TexScroll s;
            s.target = main_string(main, u32(main, o));
            if ((kind != 9 && kind != 10) || !find_texture(g, s.target, t) || (t.flags & texture_direct)) {
                continue;
            }
            for (uint32_t c : load_list_commands(g, t.data, 0xF2)) {
                s.tile_sizes.push_back({ c, u32(g, c + 4) });
            }
            if (s.tile_sizes.empty()) {
                continue;
            }
            s.t = kind == 10;
            s.position = s16(main, o + 4);
            s.wrap = s16(main, o + 6);
            s.speed = (int8_t)u8(main, o + 8);
            s.rate = s16(main, o + 0xA);
            out.scrolls.push_back(std::move(s));
        }
        return out;
    }
}

std::set<std::string> rush2::track2049::model_names(const std::vector<uint8_t>& d) {
    std::set<std::string> out;
    if (d.size() < 40) {
        return out;
    }
    uint32_t h[10];
    for (int i = 0; i < 10; i++) {
        h[i] = u32(d, i * 4);
    }
    // The header checks of model.R2Model.
    if (h[9] != 0 || (uint64_t)h[0] + (uint64_t)h[4] * 0x34 != h[2] || (uint64_t)h[2] + (uint64_t)h[5] * 0x20 != h[3] ||
        (uint64_t)h[1] + (uint64_t)h[4] * 0x18 > d.size() || h[7] > h[8] || h[8] > d.size()) {
        return out;
    }
    for (uint32_t i = 0; i < h[4]; i++) {
        out.insert(cstr(d, h[1] + (size_t)i * 0x18, 16));
    }
    return out;
}

bool rush2::track2049::rush2_lz_decompress(const uint8_t* src, size_t src_size, std::vector<uint8_t>& out) {
    out.clear();
    size_t i = 0;
    uint32_t pos = 1; // Ring buffer position of the next byte.
    while (i < src_size) {
        uint8_t flags = src[i++];
        for (int bit = 0; bit < 8; bit++) {
            if (flags & (1 << bit)) {
                if (i >= src_size) {
                    return false;
                }
                out.push_back(src[i++]);
                pos = (pos + 1) & 0xFFF;
                continue;
            }
            if (i + 2 > src_size) {
                return false;
            }
            uint8_t b0 = src[i++];
            uint8_t b1 = src[i++];
            int32_t offset = (((b0 & 0xF0) << 4) | b1) & 0xFFF;
            uint32_t length = (b0 & 0x0F) + 2;
            if (offset == 0 && length == 2) {
                return true;
            }
            int32_t distance = (int32_t)pos - offset;
            if (distance <= 0) {
                distance += 0x1000;
            }
            for (uint32_t n = 0; n < length; n++) {
                out.push_back((size_t)distance <= out.size() ? out[out.size() - distance] : 0);
            }
            pos = (pos + length) & 0xFFF;
        }
    }
    return false;
}

bool rush2::track2049::rush2_shared_model_names(const std::vector<uint8_t>& rom, uint32_t asset12_rom,
                                                uint32_t asset14_rom, std::set<std::string>& out) {
    if (asset12_rom >= rom.size() || asset14_rom >= rom.size()) {
        return false;
    }
    std::vector<uint8_t> asset12, asset14;
    if (!rush2_lz_decompress(rom.data() + asset12_rom, rom.size() - asset12_rom, asset12) ||
        !rush2::assets::inflate_raw(rom.data() + asset14_rom, rom.size() - asset14_rom, asset14)) {
        return false;
    }
    std::set<std::string> a = model_names(asset12), b = model_names(asset14);
    if (a.empty() || b.empty()) {
        return false;
    }
    out = std::move(a);
    out.insert(b.begin(), b.end());
    return true;
}

bool rush2::track2049::convert_track(const std::vector<uint8_t>& rom, int k, const std::string& prefix,
                                     const std::set<std::string>& shared_models, bool static_paths,
                                     ConvertedTrack& out, std::string& error) {
    bool race = k >= 1 && k <= 6;
    if (!race && (k < stunt_first || k >= stunt_first + stunt_count)) {
        error = "no such 2049 track";
        return false;
    }
    Bytes main;
    if (rom.size() <= main_rom || !rush2::assets::inflate_raw(rom.data() + main_rom, rom.size() - main_rom, main)) {
        error = "can't read the Rush 2049 main code";
        return false;
    }
    // 2049 files of track k (-1: none).
    const int file_numbers[] = { 100 + k, race ? 81 + k : -1, shared_model_file, 119 + k, 138 + k,
                                 157 + k, race ? 176 + k : 157 + k, coin_model_file };
    Bytes files[8];
    for (int i = 0; i < 8; i++) {
        if (file_numbers[i] >= 0 && !rush2::rom2049::read_file(rom, file_numbers[i], files[i])) {
            error = "can't read Rush 2049 file " + std::to_string(file_numbers[i]);
            return false;
        }
    }
    const Bytes& track = files[0];
    const Bytes& track_objects = files[1];
    const Bytes& shared = files[2];
    const Bytes& placement = files[3];
    const Bytes& collision = files[4];
    const Bytes& coins = files[7];

    try {
        // Models Rush 2's shared assets also define (breakable glass and flags) are left to Rush 2: placement maps
        // those objects to Rush 2's breakable classes, whose behaviour comes from Rush 2's name records. The sky gets
        // the name Rush 2's sky code looks up; FINISH models and CHKPNT/FINISH textures are looked up unchecked.
        MergeOptions opt{};
        opt.rename = { { "SKYSKY", "SKYO1" }, { "STUNTSKYSKY", "SKYO1" } };  // stunt arena 4 names its sky STUNTSKYSKY
        opt.dummies = { prefix + "FINISH", prefix + "FINISHB" };
        opt.dummy_textures = { "CHKPNT", "FINISH" };
        opt.exclude = &shared_models;
        opt.kinds = coin_kinds;
        std::vector<Type49> types = read_types(main);
        for (const Type49& t : types) {
            if (is_prop(t) && !t.model.empty()) {
                opt.rename[t.model] = prop_model(t.model);
            }
        }
        std::set<std::string> names;
        std::vector<const Bytes*> geometry_files = { &track };
        if (race) {
            geometry_files.push_back(&track_objects);
        }
        geometry_files.push_back(&shared);
        geometry_files.push_back(&coins);
        out.geometry = merge_models(geometry_files, opt, names);

        out.path_records.clear();
        out.spin_records.clear();
        out.prop_records.clear();
        out.placement = convert_placement(placement, track, prefix, types, names, static_paths, &out.path_records,
                                          &out.spin_records, &out.prop_records);
        out.collision = convert_collision(collision);
        validate_path(files[5]);
        validate_path(files[6]);
        out.path = race ? files[5] : spine_lanes(files[5], collision);
        out.path_backward = race ? files[6] : out.path;

        out.pvs_count = u8(main, pvs_counts_vram - main_vram + k - 1);
        out.pvs = convert_pvs(main, k, out.pvs_count);
        for (int i = 0; i < 3; i++) {
            out.fog[i] = u8(main, fog_colours_vram - main_vram + (k - 1) * 3 + i);
        }
        out.demo_starts[0].clear();
        out.demo_starts[1].clear();
        for (int b = 0; b < 2 && race; b++) {
            int entry = k - 1 + 6 * b;
            uint32_t list = u32(main, demo_lists_vram - main_vram + entry * 4);
            int count = s16(main, demo_counts_vram - main_vram + entry * 2);
            for (int i = 0; i < count; i++) {
                out.demo_starts[b].push_back(s16(main, list - main_vram + i * 2));
            }
        }
        out.tex_anims = convert_tex_anims(main, k, out.geometry);
    }
    catch (const ConvertError& e) {
        error = e.message;
        return false;
    }
    return true;
}

// A Rush 2049 car (files 88-100) as a Rush 2 car asset for the car type named `name` (Rush 2's car names, table
// 0x800C0764). Rush 2 builds 39 part names from the car name and the suffixes at 0x800C6180 and uses their handles
// unchecked (func_80086700): the frame, five body panels (four corners and the roof) in damage stages, and headlights.
// A 2049 car is one body that 2049 dents by moving its vertices: the body becomes the frame and the panels and lights
// are empty, so the car draws once whatever damage stage Rush 2 picks.
bool rush2::track2049::convert_car(const std::vector<uint8_t>& rom, int car, const std::string& name,
                                   std::vector<uint8_t>& out, std::string& error) {
    static const char* const parts[] = {
        "FRAME1", "D0_FR1", "D2_FR1", "D0_FL1", "D2_FL1", "D0_RR1", "D2_RR1", "D0_RL1", "D2_RL1", "D0_TOP1",
        "D1_TOP1", "D0_DFR1", "D2_DFR1", "D0_DFL1", "D2_DFL1", "D0_DRR1", "D2_DRR1", "D0_DRL1", "D2_DRL1", "D0_DTOP1",
        "D0_D2FR2", "D2_D2FR2", "D0_D2FL2", "D2_D2FL2", "D0_D2RR2", "D2_D2RR2", "D0_D2RL2", "D2_D2RL2", "D0_D2TOP2",
        "H0_L", "H2_L", "H0_R", "H2_R", "H0D_L", "H2D_L", "H0D_R", "H2D_R",
    };
    if (car < 1 || car > 13) {
        error = "no Rush 2049 car " + std::to_string(car);
        return false;
    }
    Bytes file, effects;
    if (!rush2::rom2049::read_file(rom, 87 + car, file)) {
        error = "can't read Rush 2049 file " + std::to_string(87 + car);
        return false;
    }
    try {
        std::string prefix = "CAR" + std::to_string(car);
        MergeOptions opt{};
        opt.rename = { { prefix + "FRAME1", name + "FRAME1" } };
        for (const char* part : parts) {
            opt.dummies.push_back(name + part);
        }
        // HOOD is the hood 2049 throws off a wrecked car (FRAME1 has its own), SHEEN a reflection pass Rush 2 can't
        // draw: neither is drawn.
        std::set<std::string> exclude = { prefix + "HOOD", prefix + "SHEEN" };
        std::vector<const Bytes*> files = { &file };
        // The Rocket ZX's exhaust flames: three effect models (file 62) 2049 attaches behind it (func_800AF690) and
        // rescales every frame (func_800930A4). They are drawn with the body; src/car2049.cpp places and animates them.
        Bytes flames;
        if (car == rocket_car && rush2::rom2049::read_file(rom, effects_file, effects)) {
            std::set<std::string> keep = { "ROKTFLAMEG1", "ROKTFLAMEG2", "ROKTFLAMEG3" };
            flames = subset_model49(effects, keep);
            for (const std::string& n : keep) {
                opt.appends.insert({ name + "FRAME1", n });
            }
            files.push_back(&flames);
        }
        opt.exclude = &exclude;
        std::set<std::string> names;
        out = merge_models(files, opt, names);
    }
    catch (const ConvertError& e) {
        error = e.message;
        return false;
    }
    // 2049 paints the body in the combiner's second cycle (TEXEL0 * SHADE, then * PRIM = paint colour). Rush 2 leaves
    // PRIM to whatever was drawn last, so drop the PRIM multiply; the paint is applied to the car's palette instead
    // (src/car2049.cpp).
    static const uint8_t painted[8] = { 0xFC, 0x12, 0x7E, 0x03, 0xFF, 0x0F, 0xF3, 0xFF };
    static const uint8_t unpainted[8] = { 0xFC, 0x12, 0x7F, 0xFF, 0xFF, 0xFF, 0xF2, 0x38 };
    for (size_t o = 0; o + 8 <= out.size(); o += 8) {
        if (memcmp(&out[o], painted, 8) == 0) {
            memcpy(&out[o], unpainted, 8);
        }
    }
    // 2049 lights its cars at runtime over a flat grey (0x88) vertex colour; Rush 2's car vertices are white, so do
    // the same here.
    std::set<uint32_t> seen;
    std::function<void(uint32_t)> whiten = [&](uint32_t pc) {
        for (; pc + 8 <= out.size() && seen.insert(pc).second; pc += 8) {
            uint32_t w0 = u32(out, pc), w1 = u32(out, pc + 4) & 0xFFFFFF;
            uint8_t op = uint8_t(w0 >> 24);
            if (op == 0xDF) return;
            if (op == 0xDE) whiten(w1);
            if (op == 0x01) {
                uint32_t n = (w0 >> 12) & 0xFF;
                for (uint32_t v = 0; v < n && w1 + v * 16 + 16 <= out.size(); v++) {
                    out[w1 + v * 16 + 12] = out[w1 + v * 16 + 13] = out[w1 + v * 16 + 14] = 0xFF;
                }
            }
        }
    };
    // The flames keep their own colours (their lists are marked walked; the body's lists call them).
    uint32_t models = u32(out, 0), model_names = u32(out, 4), model_count = u32(out, 16);
    for (uint32_t m = 0; m < model_count; m++) {
        uint32_t r = models + m * 0x34;
        if (cstr(out, model_names + m * 0x18, 16).rfind("ROKTFLAME", 0) == 0) {
            for (uint32_t l = 0; l < u32(out, r) && l < 4; l++) {
                seen.insert(u32(out, r + 4 + l * 12 + 8));
            }
        }
    }
    for (uint32_t m = 0; m < model_count; m++) {
        uint32_t r = models + m * 0x34;
        for (uint32_t l = 0; l < u32(out, r) && l < 4; l++) {
            whiten(u32(out, r + 4 + l * 12 + 8));
        }
    }
    return true;
}
