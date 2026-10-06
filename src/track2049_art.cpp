// Track select art for the Rush 2049 tracks, generated from the user's 2049 ROM.
//
// Rush 2's track select shows each track as a small 3D diorama (asset 3: one model per track, a textured miniature
// of the track on a wooden base with the route drawn in red) and a 128x32 name logo (assets 4-15: one CI8 texture
// each). Rush 2049's track select shows a round screenshot (file 60, TPIC1-6) with a 3D tube along the track's route
// in front of it, which its overlay builds at runtime from the AI path (docs/rush2049_research/menus.md §7). The
// 2049 entries get:
// - that route tube as the diorama (R49TRACKn, build_tube): 2049's construction and colours, ported;
// - or, with use_miniature, a miniature of the track itself: the track's own geometry (file 100+k, every object the
//   placement file 119+k places, LOD 0, no sky), cropped to the route's extent plus a margin, shrunk so the longer
//   side is 1120 model units like the stock dioramas, on a wooden base, with the route in red on top (the AI path's
//   spine and the branches that really leave it). The notes below up to "Size" describe this miniature;
// - a logo with the track's screenshot shrunk to an icon and its name, "TRACK n" as Rush 2049's code calls them.
// The stunt arenas (k = stunt_first.., src/track2049_convert.cpp) get the same from their own AI path (157+k) and
// screenshot (SPICn), named R49STUNTn / R49SLOGOn and "STUNT n", and the obstacle course (k = obstacle) from its
// path (176) and OPIC1, named R49OBSTACLE / R49OLOGO and "OBSTACLE". The miniature is for race tracks only.
//
// Both go into a copy of asset 3, whose tables are rebuilt after the appended data; the screen looks models and
// textures up by name, so the copy works wherever asset 3 is loaded.
//
// Rush 2 model container (docs/rush2049_research/geometry.md): a 10-word header, data, then the model (0x34 bytes),
// texture (0x20) and palette (0x18) tables back to back and the name table (0x18) last. Names are kept sorted, since
// lookups are binary searches. The loader rebases pointers in each model list it walks (func_8007786C: VTX, DL,
// SETTIMG...; it doesn't enter G_DL targets) and the SETTIMGs in the texture-load range [7]..[8] (func_80077B38).
//
// The miniature is one model with one list, so the relocator walks all of it:
// - Each placed object's list is copied command by command with its G_ENDDL dropped. The 2049 vertices are baked:
//   transformed by the placement (position + matrix, vertices in 1/16 units) and the diorama scale into s16 model
//   coordinates, so no G_MTX is needed (nothing to compose with func_8007AA48's matrix or the interpolation's matrix
//   tagging). Triangles are re-batched into fresh G_VTX loads of only the vertices they use.
// - What is left out: shapes (connected triangles) smaller than a few dozen world units away from the route (poles,
//   lamps, signs, cones, clutter), thin shapes (wires, cables, rails: little area for their length), translucent
//   panels that aren't horizontal (glass, light shafts; translucent water stays), faces whose front side looks down,
//   triangles too small to see, and everything outside the crop (vertices on the edge are clamped to it). Objects use
//   LOD 0: the far LODs some objects have are impostor cards. If a track comes out over track_budget, it is rebuilt
//   with stricter limits (detail_levels); near the route the finest limits always apply.
// - Texture-load lists (2049 TXLD) are copied once each into the container's texture-load range, which is grown by
//   inserting them right after Rush 2's own (Rush 2's data after that point moves up, with its pointers fixed the way
//   the relocator finds them). Only the texels and palettes they load are copied, with the finest mip levels left out
//   (levels_dropped; every 2049 track texture is a mip chain, see Miniature::drop_levels).
// - The 2049 lists keep their render modes, combiners and E0 01 mirror conditionals (retargeted, resolved by the
//   loader as in a race), except mipmapping: on the small miniature the RDP would pick the coarsest levels and flatten
//   every texture to one colour, so texture LOD is turned off and the mip combiner becomes TEXEL0 * SHADE (tile 0, the
//   finest level kept, is drawn, as the stock dioramas draw theirs). No 2049 track LOD is lit, so the objects are
//   unlit like the stock dioramas.
// - State: the list starts like VEGASTRACK (G_TEXTURE off, render mode E200001C C8112078, shade combiner) for the
//   route and the base, then textures on (CI8 64x32 loads like the stock dioramas', combiner TEXEL0 * SHADE) for the
//   wooden skirt, then the objects. It ends restoring what Rush 2 expects and the 2049 lists change: TEXTLUT RGBA16
//   (track2049_convert.cpp), the blend colour (if an object set it), alpha compare off (if an object set it) and
//   texture LOD off (as VEGASTRACK ends). 2049's RGB dither setting is left as the lists leave it (cosmetic, as in
//   races). Every vertex is chained for the loader's mirror pass (bit 15 of +6); the route and base come in both
//   windings.
//
// Size: the tube is 10 KB per track (asset 3 grows to 0.57 MB) and builds in a few ms. The miniature is about
// 0.63-0.77 MB per track (asset 3 about 4.8 MB; the heap is 7 MB) and takes about 0.2 s for the six tracks in an
// optimized build. Built once per ROM; nothing is cached.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <functional>
#include <map>
#include <set>
#include <unordered_map>
#include <string>

#include "rush2049_rom.h"
#include "track2049.h"

namespace {
    constexpr int path_file = 157;      // + k: forward AI path of race track k or stunt arena k.
    constexpr int geometry_file = 100;  // + k: track geometry.
    constexpr int placement_file = 119; // + k: object placement.
    constexpr int ui_file = 60;         // Track select thumbnails TPIC1-6.

    using Bytes = std::vector<uint8_t>;

    bool is_obstacle(int k) { return k == rush2::track2049::obstacle; }
    bool is_stunt(int k) { return k >= rush2::track2049::stunt_first && !is_obstacle(k); }
    // The number in the track's name: race track k, stunt arena 1-4, or 1 for the obstacle course.
    int number_of(int k) { return is_stunt(k) ? k - rush2::track2049::stunt_first + 1 : is_obstacle(k) ? 1 : k; }

    uint32_t be32(const std::vector<uint8_t>& d, size_t o) {
        return (uint32_t(d[o]) << 24) | (uint32_t(d[o + 1]) << 16) | (uint32_t(d[o + 2]) << 8) | d[o + 3];
    }
    uint16_t be16(const std::vector<uint8_t>& d, size_t o) {
        return uint16_t((d[o] << 8) | d[o + 1]);
    }
    void put32(std::vector<uint8_t>& d, size_t o, uint32_t v) {
        d[o] = uint8_t(v >> 24); d[o + 1] = uint8_t(v >> 16); d[o + 2] = uint8_t(v >> 8); d[o + 3] = uint8_t(v);
    }
    void push32(std::vector<uint8_t>& d, uint32_t v) {
        d.push_back(uint8_t(v >> 24)); d.push_back(uint8_t(v >> 16)); d.push_back(uint8_t(v >> 8)); d.push_back(uint8_t(v));
    }
    void push16(std::vector<uint8_t>& d, uint16_t v) {
        d.push_back(uint8_t(v >> 8)); d.push_back(uint8_t(v));
    }
    void align(std::vector<uint8_t>& d, size_t n) {
        while (d.size() % n) {
            d.push_back(0);
        }
    }
    float as_float(uint32_t v) {
        float f;
        memcpy(&f, &v, 4);
        return f;
    }
    uint32_t float_bits(float f) {
        uint32_t v;
        memcpy(&v, &f, 4);
        return v;
    }

    // 2049 chunk directory (geometry.md §3): word 0 is the offset of {tag, offset, size or count} entries.
    struct Chunk {
        uint32_t offset = 0, size = 0;
        bool found = false;
    };
    std::map<std::string, Chunk> chunks49(const Bytes& d) {
        std::map<std::string, Chunk> out;
        if (d.size() < 4) return out;
        for (uint32_t o = be32(d, 0); o + 12 <= d.size(); o += 12) {
            std::string tag(reinterpret_cast<const char*>(&d[o]), 4);
            if (!std::all_of(tag.begin(), tag.end(), ::isalpha)) break;
            out[tag] = { be32(d, o + 4), be32(d, o + 8), true };
        }
        return out;
    }

    // ---------------------------------------------------------------------------------------------------------------
    // Software RDP: runs F3DEX2 model lists of a container (2049 or Rush 2) and hands out textured triangles.

    struct RdpVertex {
        float x, y, z;  // transformed by the caller
        float s, t;     // texture coordinates, s10.5 as in the vertex
        float c[4];     // vertex colour 0..1
    };

    class Rdp {
    public:
        // d: the container. SETTIMG addresses in lists inside [img_lo, img_hi) are relative to img_base (2049's TXLD
        // lists are IMAG-relative); everything else is file-relative.
        Rdp(const Bytes& d, uint32_t img_lo = 0, uint32_t img_hi = 0, uint32_t img_base = 0)
            : d(d), img_lo(img_lo), img_hi(img_hi), img_base(img_base) {
            memset(tmem, 0, sizeof(tmem));
            memset(tlut, 0, sizeof(tlut));
        }

        std::function<void(int16_t x, int16_t y, int16_t z, float out[3])> transform;
        std::function<void(const RdpVertex& a, const RdpVertex& b, const RdpVertex& c)> triangle;

        uint32_t render_mode = 0;      // othermode L
        uint32_t othermode_h = 0x00100000;  // 2-cycle, as Rush 2 and 2049 draw models
        bool texture_on = false;
        int errors = 0;                // out-of-range addresses, runaway lists
        size_t vertices_loaded = 0, triangles = 0;

        // Runs the list at dl. flags: the load flags of the E0 01 conditional op (bit 1 one player, 3 not mirrored).
        void run(uint32_t dl, uint32_t flags) {
            uint32_t stack[16];
            int sp = 0;
            uint32_t pc = dl;
            for (int guard = 0; guard < 1000000; guard++) {
                if (pc + 8 > d.size()) { errors++; return; }
                uint32_t w0 = be32(d, pc), w1 = be32(d, pc + 4);
                uint32_t here = pc;
                pc += 8;
                uint8_t op = uint8_t(w0 >> 24);
                switch (op) {
                case 0x01: { // G_VTX
                    uint32_t n = (w0 >> 12) & 0xFF, end = (w0 >> 1) & 0x7F;
                    uint32_t addr = w1 & 0xFFFFFF;
                    if (n > end || end > 64 || addr + n * 16 > d.size()) { errors++; break; }
                    for (uint32_t i = 0; i < n; i++) {
                        uint32_t a = addr + i * 16;
                        RdpVertex& v = vbuf[end - n + i];
                        float p[3];
                        transform(int16_t(be16(d, a)), int16_t(be16(d, a + 2)), int16_t(be16(d, a + 4)), p);
                        v.x = p[0]; v.y = p[1]; v.z = p[2];
                        v.s = float(int16_t(be16(d, a + 8)));
                        v.t = float(int16_t(be16(d, a + 10)));
                        for (int k = 0; k < 4; k++) v.c[k] = d[a + 12 + k] / 255.0f;
                    }
                    vertices_loaded += n;
                    break;
                }
                case 0x05: tri(w0); break;
                case 0x06: case 0x07: tri(w0); tri(w1); break;
                case 0xD7: // G_TEXTURE
                    scale_s = (w1 >> 16) / 65536.0f; scale_t = (w1 & 0xFFFF) / 65536.0f;
                    tex_tile = (w0 >> 8) & 7; texture_on = (w0 & 0xFF) != 0;
                    break;
                case 0xDE: // G_DL
                    if (((w0 >> 16) & 0xFF) == 0) {
                        if (sp >= 16) { errors++; return; }
                        stack[sp++] = pc;
                    }
                    pc = w1 & 0xFFFFFF;
                    break;
                case 0xDF: // G_ENDDL
                    if (sp == 0) return;
                    pc = stack[--sp];
                    break;
                case 0xE0: // conditional (geometry.md §4.1): skip to the target unless load flag bit c is set
                    if (((w0 >> 16) & 0xFF) == 1 && !(flags & (1u << (w0 & 0xFFFF)))) pc = w1 & 0xFFFFFF;
                    break;
                case 0xE2: othermode(render_mode, w0, w1); break;
                case 0xE3: othermode(othermode_h, w0, w1); break;
                case 0xFA: for (int k = 0; k < 4; k++) prim[k] = ((w1 >> (24 - 8 * k)) & 0xFF) / 255.0f; break;
                case 0xFB: for (int k = 0; k < 4; k++) env[k] = ((w1 >> (24 - 8 * k)) & 0xFF) / 255.0f; break;
                case 0xFC: combine0 = w0 & 0xFFFFFF; combine1 = w1; break;
                case 0xFD: // G_SETTIMG
                    img_siz = (w0 >> 19) & 3;
                    img_width = (w0 & 0xFFF) + 1;
                    img_addr = (w1 & 0xFFFFFF) + (here >= img_lo && here < img_hi ? img_base : 0);
                    break;
                case 0xF5: { // G_SETTILE
                    Tile& t = tiles[(w1 >> 24) & 7];
                    t.fmt = (w0 >> 21) & 7; t.siz = (w0 >> 19) & 3; t.line = (w0 >> 9) & 0x1FF; t.tmem = w0 & 0x1FF;
                    t.pal = (w1 >> 20) & 0xF;
                    t.cmt = (w1 >> 18) & 3; t.maskt = (w1 >> 14) & 0xF; t.shiftt = (w1 >> 10) & 0xF;
                    t.cms = (w1 >> 8) & 3; t.masks = (w1 >> 4) & 0xF; t.shifts = w1 & 0xF;
                    break;
                }
                case 0xF2: { // G_SETTILESIZE
                    Tile& t = tiles[(w1 >> 24) & 7];
                    t.uls = (w0 >> 12) & 0xFFF; t.ult = w0 & 0xFFF; t.lrs = (w1 >> 12) & 0xFFF; t.lrt = w1 & 0xFFF;
                    break;
                }
                case 0xF3: { // G_LOADBLOCK
                    const Tile& t = tiles[(w1 >> 24) & 7];
                    uint32_t uls = (w0 >> 12) & 0xFFF, lrs = (w1 >> 12) & 0xFFF;
                    uint32_t bits = 4u << img_siz;
                    uint32_t bytes = ((lrs - std::min(uls, lrs) + 1) * bits + 7) / 8;
                    copy_tmem(t.tmem * 8, img_addr + uls * bits / 8, bytes);
                    break;
                }
                case 0xF4: { // G_LOADTILE
                    Tile& t = tiles[(w1 >> 24) & 7];
                    t.uls = (w0 >> 12) & 0xFFF; t.ult = w0 & 0xFFF; t.lrs = (w1 >> 12) & 0xFFF; t.lrt = w1 & 0xFFF;
                    uint32_t bits = 4u << img_siz;
                    uint32_t s0 = t.uls >> 2, s1 = t.lrs >> 2, t0 = t.ult >> 2, t1 = t.lrt >> 2;
                    for (uint32_t row = t0; row <= t1 && row < t0 + 1024; row++) {
                        copy_tmem(t.tmem * 8 + (row - t0) * t.line * 8, img_addr + (row * img_width + s0) * bits / 8,
                                  ((s1 - std::min(s0, s1) + 1) * bits + 7) / 8);
                    }
                    break;
                }
                case 0xF0: { // G_LOADTLUT
                    const Tile& t = tiles[(w1 >> 24) & 7];
                    uint32_t first = ((w0 >> 12) & 0xFFF) >> 2, last = ((w1 >> 12) & 0xFFF) >> 2;
                    int start = int(t.tmem) - 0x100;
                    for (uint32_t i = first; i <= last && i < first + 256; i++) {
                        uint32_t a = img_addr + i * 2;
                        int slot = start + int(i - first);
                        if (a + 2 > d.size()) { errors++; break; }
                        if (slot >= 0 && slot < 256) tlut[slot] = be16(d, a);
                    }
                    break;
                }
                default: break; // syncs, geometry mode, scissor...: nothing to emulate
                }
            }
            errors++;
        }

        // Texture sample and colour combiner for a fragment with interpolated s, t (s10.5) and vertex colour.
        void shade(float s, float t, const float c[4], float out[4]) const {
            float t0[4] = { 1, 1, 1, 1 };
            if (texture_on) {
                texel(tex_tile, s * scale_s / 32.0f, t * scale_t / 32.0f, t0);
            }
            const float* t1 = t0; // the next mip level; close enough
            float comb[4] = { 0, 0, 0, 0 };
            bool two_cycle = ((othermode_h >> 20) & 3) == 1;
            for (int cycle = two_cycle ? 0 : 1; cycle < 2; cycle++) {
                int a, b, cc, dd, aa, ab, ac, ad;
                if (cycle == 0) {
                    a = (combine0 >> 20) & 0xF; cc = (combine0 >> 15) & 0x1F; aa = (combine0 >> 12) & 7; ac = (combine0 >> 9) & 7;
                    b = (combine1 >> 28) & 0xF; dd = (combine1 >> 15) & 7; ab = (combine1 >> 12) & 7; ad = (combine1 >> 9) & 7;
                }
                else {
                    a = (combine0 >> 5) & 0xF; cc = combine0 & 0x1F; aa = (combine1 >> 21) & 7; ac = (combine1 >> 18) & 7;
                    b = (combine1 >> 24) & 0xF; dd = (combine1 >> 6) & 7; ab = (combine1 >> 3) & 7; ad = combine1 & 7;
                }
                auto src = [&](int sel) -> const float* {
                    static const float one[4] = { 1, 1, 1, 1 }, zero[4] = { 0, 0, 0, 0 };
                    switch (sel) {
                    case 0: return comb;
                    case 1: return t0;
                    case 2: return t1;
                    case 3: return prim;
                    case 4: return c;
                    case 5: return env;
                    case 6: return one;
                    default: return zero;
                    }
                };
                float res[4];
                for (int k = 0; k < 3; k++) {
                    float va = a < 7 ? src(a)[k] : 0.0f;          // 7 = noise
                    float vb = b < 6 ? src(b)[k] : 0.0f;
                    float vc;
                    if (cc < 6) vc = src(cc)[k];
                    else if (cc == 7) vc = comb[3];
                    else if (cc >= 8 && cc <= 12) vc = src(cc - 7)[3];
                    else vc = 0.0f;                                // key scale, LOD fractions, K5
                    float vd = src(dd)[k];
                    res[k] = (va - vb) * vc + vd;
                }
                float va = src(aa)[3], vb = src(ab)[3], vd = src(ad)[3];
                float vc = ac == 0 || ac == 6 ? 0.0f : src(ac)[3];
                res[3] = (va - vb) * vc + vd;
                for (int k = 0; k < 4; k++) comb[k] = std::clamp(res[k], 0.0f, 1.0f);
            }
            memcpy(out, comb, sizeof(comb));
        }

        // Point-sampled texel of a tile at texel coordinates s, t.
        void texel(int tile, float fs, float ft, float out[4]) const {
            const Tile& tl = tiles[tile];
            int s = int(std::floor(fs)), t = int(std::floor(ft));
            auto shift = [](int v, int sh) { return sh == 0 ? v : sh <= 10 ? v >> sh : v << (16 - sh); };
            s = shift(s, tl.shifts) - int(tl.uls >> 2);
            t = shift(t, tl.shiftt) - int(tl.ult >> 2);
            auto wrap = [](int v, int cm, int mask, int size) {
                if ((cm & 2) || mask == 0) v = std::clamp(v, 0, std::max(size, 0));
                if (mask) {
                    if ((cm & 1) && ((v >> mask) & 1)) v = ~v;
                    v &= (1 << mask) - 1;
                }
                return v;
            };
            s = wrap(s, tl.cms, tl.masks, int((tl.lrs >> 2) - (tl.uls >> 2)));
            t = wrap(t, tl.cmt, tl.maskt, int((tl.lrt >> 2) - (tl.ult >> 2)));
            uint32_t base = tl.tmem * 8 + uint32_t(t) * tl.line * 8;
            auto b8 = [&](uint32_t a) { return tmem[a & 0xFFF]; };
            auto b16 = [&](uint32_t a) { return uint16_t((b8(a) << 8) | b8(a + 1)); };
            auto c5551 = [&](uint16_t v) {
                out[0] = ((v >> 11) & 31) / 31.0f; out[1] = ((v >> 6) & 31) / 31.0f; out[2] = ((v >> 1) & 31) / 31.0f;
                out[3] = float(v & 1);
            };
            auto ia = [&](float i, float a) { out[0] = out[1] = out[2] = i; out[3] = a; };
            int nib = tl.siz == 0 ? ((b8(base + s / 2) >> ((s & 1) ? 0 : 4)) & 0xF) : 0;
            switch (tl.fmt) {
            case 0: // RGBA
                if (tl.siz == 3) {
                    uint32_t a = base + s * 4;
                    for (int k = 0; k < 4; k++) out[k] = b8(a + k) / 255.0f;
                }
                else c5551(b16(base + s * 2));
                break;
            case 2: // CI
                if (tl.siz == 0) c5551(tlut[(tl.pal * 16 + nib) & 0xFF]);
                else c5551(tlut[b8(base + s)]);
                break;
            case 3: // IA
                if (tl.siz == 0) ia((nib >> 1) / 7.0f, float(nib & 1));
                else if (tl.siz == 1) { uint8_t v = b8(base + s); ia((v >> 4) / 15.0f, (v & 15) / 15.0f); }
                else { uint16_t v = b16(base + s * 2); ia((v >> 8) / 255.0f, (v & 0xFF) / 255.0f); }
                break;
            case 4: // I
                if (tl.siz == 0) ia(nib / 15.0f, nib / 15.0f);
                else { float v = b8(base + s) / 255.0f; ia(v, v); }
                break;
            default:
                ia(0.5f, 1.0f);
                break;
            }
        }

    private:
        struct Tile {
            uint32_t fmt = 0, siz = 0, line = 0, tmem = 0, pal = 0, cms = 0, cmt = 0, masks = 0, maskt = 0, shifts = 0,
                     shiftt = 0, uls = 0, ult = 0, lrs = 0, lrt = 0;
        };
        const Bytes& d;
        uint32_t img_lo, img_hi, img_base;
        uint8_t tmem[4096];
        uint16_t tlut[256];
        Tile tiles[8];
        uint32_t img_addr = 0, img_siz = 0, img_width = 1;
        float scale_s = 1, scale_t = 1;
        int tex_tile = 0;
        uint32_t combine0 = 0xFFFFFF, combine1 = 0xFFFE7C38;
        float prim[4] = { 1, 1, 1, 1 }, env[4] = { 1, 1, 1, 1 };
        RdpVertex vbuf[64];

        void tri(uint32_t w) {
            int a = ((w >> 16) & 0xFF) / 2, b = ((w >> 8) & 0xFF) / 2, c = (w & 0xFF) / 2;
            if (a >= 64 || b >= 64 || c >= 64) { errors++; return; }
            triangles++;
            if (triangle) triangle(vbuf[a], vbuf[b], vbuf[c]);
        }
        static void othermode(uint32_t& mode, uint32_t w0, uint32_t w1) {
            uint32_t len = (w0 & 0xFF) + 1, shift = 32 - ((w0 >> 8) & 0xFF) - len;
            uint32_t mask = (len >= 32 ? 0xFFFFFFFFu : ((1u << len) - 1)) << shift;
            mode = (mode & ~mask) | (w1 & mask);
        }
        void copy_tmem(uint32_t dst, uint32_t src, uint32_t bytes) {
            if (src + bytes > d.size()) { errors++; return; }
            for (uint32_t i = 0; i < bytes; i++) tmem[(dst + i) & 0xFFF] = d[src + i];
        }
    };

    // Rasterizes a triangle given in pixel coordinates (any winding); calls pixel(x, y, l0, l1, l2) for each pixel
    // centre inside, with barycentric weights.
    template <class F>
    void raster(const float* p0, const float* p1, const float* p2, int w, int h, F&& pixel) {
        float area = (p1[0] - p0[0]) * (p2[1] - p0[1]) - (p2[0] - p0[0]) * (p1[1] - p0[1]);
        if (std::fabs(area) < 1e-6f) return;
        int x0 = std::max(0, int(std::floor(std::min({ p0[0], p1[0], p2[0] })))),
            x1 = std::min(w - 1, int(std::ceil(std::max({ p0[0], p1[0], p2[0] })))),
            y0 = std::max(0, int(std::floor(std::min({ p0[1], p1[1], p2[1] })))),
            y1 = std::min(h - 1, int(std::ceil(std::max({ p0[1], p1[1], p2[1] }))));
        float inv = 1.0f / area;
        for (int y = y0; y <= y1; y++) {
            float py = y + 0.5f;
            for (int x = x0; x <= x1; x++) {
                float px = x + 0.5f;
                float l0 = ((p1[0] - px) * (p2[1] - py) - (p2[0] - px) * (p1[1] - py)) * inv;
                float l1 = ((p2[0] - px) * (p0[1] - py) - (p0[0] - px) * (p2[1] - py)) * inv;
                float l2 = 1.0f - l0 - l1;
                if (l0 < 0 || l1 < 0 || l2 < 0) continue;
                pixel(x, y, l0, l1, l2);
            }
        }
    }

    // ---------------------------------------------------------------------------------------------------------------
    // Top-down render of a 2049 track

    struct Point {
        float x, y, z;
    };

    // Spine (centre line) and branch points of an AI path file (docs/rush2049_research/race.md §3).
    bool read_path(const std::vector<uint8_t>& path, std::vector<Point>& spine, std::vector<std::vector<Point>>& branches) {
        constexpr size_t route = 0x32C;
        if (path.size() < route + 16) {
            return false;
        }
        int spine_count = be16(path, route);
        int branch_count = path[route + 8];
        size_t o = route + 16;
        std::vector<int> branch_counts;
        std::vector<bool> branch_kept;
        for (int i = 0; i < branch_count; i++) {
            if (o + i * 16 + 0xC > path.size()) return false;
            branch_counts.push_back(be16(path, o + i * 16 + 0xA));
            branch_kept.push_back(path[o + i * 16] != 2); // kind 2 (track 2's last) is a closed loop off the route
        }
        o += 16 * branch_count + 2;
        auto point = [&](size_t at) {
            return Point{ float(int16_t(be16(path, at))), float(int16_t(be16(path, at + 2))), float(int16_t(be16(path, at + 4))) };
        };
        if (o + 6 * size_t(spine_count) > path.size()) {
            return false;
        }
        for (int i = 0; i < spine_count; i++, o += 6) {
            spine.push_back(point(o));
        }
        for (size_t b = 0; b < branch_counts.size(); b++) {
            int n = branch_counts[b];
            if (o + 6 * size_t(n) > path.size()) {
                return false;
            }
            std::vector<Point> pts;
            for (int i = 0; i < n; i++, o += 6) {
                pts.push_back(point(o));
            }
            if (branch_kept[b]) branches.push_back(std::move(pts));
        }
        return spine.size() >= 2;
    }

    // The image is cut into 64x32 tiles that overlap by one pixel, so each tile's geometry spans 63x31 pixels and
    // bilinear filtering is seamless across tiles.
    constexpr int tile_w = 64, tile_h = 32, tile_step_x = tile_w - 1, tile_step_y = tile_h - 1;
    constexpr int supersample = 3;     // rendered at 3x3 samples per pixel
    constexpr float vertex_scale = 1.0f / 16.0f; // 2049 model vertices are in 1/16 world units
    constexpr float route_radius = 40.0f;       // world units around the AI route kept clear from above...
    constexpr float route_clearance = 20.0f;    // ...of anything higher than this over the road

    struct TopDown {
        int nx = 0, ny = 0;             // tiles
        int w = 0, h = 0;               // pixels: nx * 63 + 1, ny * 31 + 1
        float x0 = 0, z0 = 0, pixel = 1; // world x, z of pixel (0, 0)'s centre; world units per pixel
        float cx = 0, cz = 0;           // centre of the area
        float area[4] = { 0, 0, 0, 0 }; // the area: route extent plus a margin, world x0, x1, z0, z1
        float base_y = 0;               // lowest route point
        std::vector<std::array<uint8_t, 3>> rgb;  // w x h, row = +z
        std::vector<float> height;      // (w * ss) x (h * ss) world y, -inf where nothing was drawn
        std::vector<Point> spine;
        std::vector<std::vector<Point>> branches;
        bool closed = true;             // the spine is a loop (track 6 isn't: it ends far from its start)
        size_t triangles = 0;
    };

    // Renders the placed geometry around the route from above. tiles: the image's size in 64x32 tiles at most.
    bool render_top_down(const Bytes& geometry, const Bytes& placement, const Bytes& path, int tiles, TopDown& td) {
        if (!read_path(path, td.spine, td.branches)) {
            return false;
        }
        td.closed = std::hypot(td.spine.front().x - td.spine.back().x, td.spine.front().z - td.spine.back().z) < 100.0f;
        auto ch = chunks49(geometry);
        if (!ch["IMAG"].found || !ch["TXLD"].found || !ch["OBHD"].found) {
            return false;
        }

        // Area: the route's extent plus a margin, fitted to whole tiles.
        float min_x = 1e9f, max_x = -1e9f, min_z = 1e9f, max_z = -1e9f;
        td.base_y = 1e9f;
        auto extend = [&](const Point& p) {
            min_x = std::min(min_x, p.x); max_x = std::max(max_x, p.x);
            min_z = std::min(min_z, p.z); max_z = std::max(max_z, p.z);
            td.base_y = std::min(td.base_y, p.y);
        };
        for (const Point& p : td.spine) extend(p);
        for (const auto& b : td.branches) for (const Point& p : b) extend(p);
        float margin = 0.08f * std::max(max_x - min_x, max_z - min_z) + 100.0f;
        float area_w = max_x - min_x + 2 * margin, area_d = max_z - min_z + 2 * margin;
        td.area[0] = min_x - margin; td.area[1] = max_x + margin;
        td.area[2] = min_z - margin; td.area[3] = max_z + margin;
        td.pixel = 1e9f;
        for (int nx = 1; nx <= tiles; nx++) {
            for (int ny = 1; nx * ny <= tiles; ny++) {
                float p = std::max(area_w / (tile_step_x * nx), area_d / (tile_step_y * ny));
                if (p < td.pixel * 0.999f) {
                    td.pixel = p; td.nx = nx; td.ny = ny;
                }
            }
        }
        td.w = td.nx * tile_step_x + 1;
        td.h = td.ny * tile_step_y + 1;
        td.cx = (min_x + max_x) * 0.5f;
        td.cz = (min_z + max_z) * 0.5f;
        td.x0 = td.cx - (td.w - 1) * td.pixel * 0.5f;
        td.z0 = td.cz - (td.h - 1) * td.pixel * 0.5f;

        // Supersampled colour and height buffers. Sample X covers world x0 - pixel/2 + (X .. X+1) * pixel/ss.
        const int ss = supersample, sw = td.w * ss, sh = td.h * ss;
        const float sample = td.pixel / ss, left = td.x0 - td.pixel * 0.5f, top = td.z0 - td.pixel * 0.5f;
        std::vector<std::array<float, 3>> colour(size_t(sw) * sh, { 0, 0, 0 });
        td.height.assign(size_t(sw) * sh, -INFINITY);

        // Clearance over the route: anything more than route_clearance above the road within route_radius of it
        // (tunnel roofs, overpasses, signs and lamps over the road) is left out, so the road shows from above.
        std::vector<float> ceiling(size_t(sw) * sh, INFINITY);
        auto clear_route = [&](const std::vector<Point>& pts, bool closed) {
            int r = int(std::ceil(route_radius / sample));
            for (size_t i = 0; i + (closed ? 0 : 1) < pts.size(); i++) {
                const Point& a = pts[i];
                const Point& b = pts[(i + 1) % pts.size()];
                int steps = std::max(1, int(std::hypot(b.x - a.x, b.z - a.z) / (sample * 2)));
                for (int st = 0; st < steps; st++) {
                    float f = float(st) / steps;
                    float x = a.x + (b.x - a.x) * f, y = a.y + (b.y - a.y) * f, z = a.z + (b.z - a.z) * f;
                    int px = int((x - left) / sample), pz = int((z - top) / sample);
                    for (int dz = -r; dz <= r; dz++) {
                        for (int dx = -r; dx <= r; dx++) {
                            int qx = px + dx, qz = pz + dz;
                            if (qx < 0 || qz < 0 || qx >= sw || qz >= sh || (dx * dx + dz * dz) * sample * sample > route_radius * route_radius) continue;
                            float& c = ceiling[size_t(qz) * sw + qx];
                            c = c == INFINITY ? y + route_clearance : std::max(c, y + route_clearance);
                        }
                    }
                }
            }
        };
        clear_route(td.spine, td.closed);
        for (const auto& b : td.branches) clear_route(b, false);

        // Objects by name (OBHD, 0x58 bytes: name, radius, kind, LOD count, 4 x {texture, flags, distance, list,
        // vertices}); LOD 0 is the most detailed.
        struct Object { uint32_t list; float radius; };
        std::map<std::string, Object> objects;
        const Chunk& obhd = ch["OBHD"];
        for (uint32_t i = 0; i < obhd.size; i++) {
            uint32_t r = obhd.offset + i * 0x58;
            if (r + 0x58 > geometry.size()) return false;
            std::string name(reinterpret_cast<const char*>(&geometry[r]), strnlen(reinterpret_cast<const char*>(&geometry[r]), 15));
            uint32_t list = be32(geometry, r + 0x18 + 8);
            if (int16_t(be16(geometry, r + 0x16)) >= 1 && list != 0 && name != "SKYSKY") {
                objects[name] = { list, as_float(be32(geometry, r + 16)) };
            }
        }

        const Chunk& txld = ch["TXLD"];
        Rdp rdp(geometry, txld.offset, txld.offset + txld.size, ch["IMAG"].offset);
        float m[9], pos[3];
        rdp.transform = [&](int16_t x, int16_t y, int16_t z, float out[3]) {
            float v[3] = { x * vertex_scale, y * vertex_scale, z * vertex_scale };
            for (int j = 0; j < 3; j++) {
                out[j] = pos[j] + v[0] * m[j] + v[1] * m[3 + j] + v[2] * m[6 + j];
            }
        };
        rdp.triangle = [&](const RdpVertex& a, const RdpVertex& b, const RdpVertex& c) {
            float p0[2] = { (a.x - left) / sample, (a.z - top) / sample };
            float p1[2] = { (b.x - left) / sample, (b.z - top) / sample };
            float p2[2] = { (c.x - left) / sample, (c.z - top) / sample };
            uint32_t rm = rdp.render_mode;
            bool translucent = (rm & 0x4000) && !(rm & 0x20);           // FORCE_BL without Z_UPD
            bool cutout = (rm & 3) == 1 || (rm & 0x1000);                  // alpha compare or CVG_X_ALPHA
            raster(p0, p1, p2, sw, sh, [&](int x, int y, float l0, float l1, float l2) {
                size_t i = size_t(y) * sw + x;
                float wy = a.y * l0 + b.y * l1 + c.y * l2;
                if (wy < td.height[i] - 1.0f || wy > ceiling[i]) return;
                float s = a.s * l0 + b.s * l1 + c.s * l2, t = a.t * l0 + b.t * l1 + c.t * l2;
                float col[4];
                for (int q = 0; q < 4; q++) col[q] = a.c[q] * l0 + b.c[q] * l1 + c.c[q] * l2;
                float out[4];
                rdp.shade(s, t, col, out);
                if (translucent) {
                    if (td.height[i] == -INFINITY) {
                        for (int q = 0; q < 3; q++) colour[i][q] = out[q];
                        td.height[i] = wy;
                    }
                    else {
                        for (int q = 0; q < 3; q++) colour[i][q] += (out[q] - colour[i][q]) * out[3];
                    }
                    return;
                }
                if (cutout && out[3] < 0.5f) return;
                for (int q = 0; q < 3; q++) colour[i][q] = out[q];
                td.height[i] = wy;
            });
        };

        // Every record the placement file places with a geometry model: WOBJ records of 0x68 bytes {name[16],
        // f32 m[9], f32 pos[3], ...}, positions in world space (docs/rush2049_research/placement.md).
        if (placement.size() < 8) return false;
        uint32_t dir = be32(placement, 0), dir_count = be32(placement, 4);
        uint32_t wobj = 0, wobj_count = 0;
        for (uint32_t i = 0; i < dir_count && dir + i * 12 + 12 <= placement.size(); i++) {
            if (memcmp(&placement[dir + i * 12], "WOBJ", 4) == 0) {
                wobj = be32(placement, dir + i * 12 + 4);
                wobj_count = be32(placement, dir + i * 12 + 8);
            }
        }
        if (wobj_count == 0 || wobj + size_t(wobj_count) * 0x68 > placement.size()) return false;
        float x_lo = left, x_hi = left + sw * sample, z_lo = top, z_hi = top + sh * sample;
        for (uint32_t i = 0; i < wobj_count; i++) {
            uint32_t r = wobj + i * 0x68;
            std::string name(reinterpret_cast<const char*>(&placement[r]), strnlen(reinterpret_cast<const char*>(&placement[r]), 15));
            auto it = objects.find(name);
            if (it == objects.end()) continue;
            for (int j = 0; j < 9; j++) m[j] = as_float(be32(placement, r + 0x10 + j * 4));
            for (int j = 0; j < 3; j++) pos[j] = as_float(be32(placement, r + 0x34 + j * 4));
            float reach = it->second.radius * 3.0f;
            if (pos[0] + reach < x_lo || pos[0] - reach > x_hi || pos[2] + reach < z_lo || pos[2] - reach > z_hi) continue;
            rdp.render_mode = 0;
            rdp.run(it->second.list, 0x02 | 0x08); // one player, not mirrored
        }
        td.triangles = rdp.triangles;

        // Down to one colour per pixel; pixels nothing covered take their neighbours' colour.
        td.rgb.assign(size_t(td.w) * td.h, { 0, 0, 0 });
        std::vector<uint8_t> have(size_t(td.w) * td.h, 0);
        for (int y = 0; y < td.h; y++) {
            for (int x = 0; x < td.w; x++) {
                float sum[3] = { 0, 0, 0 };
                int n = 0;
                for (int sy = 0; sy < ss; sy++) {
                    for (int sx = 0; sx < ss; sx++) {
                        size_t i = size_t(y * ss + sy) * sw + x * ss + sx;
                        if (td.height[i] == -INFINITY) continue;
                        for (int q = 0; q < 3; q++) sum[q] += colour[i][q];
                        n++;
                    }
                }
                if (n) {
                    for (int q = 0; q < 3; q++) td.rgb[size_t(y) * td.w + x][q] = uint8_t(std::lround(sum[q] / n * 255.0f));
                    have[size_t(y) * td.w + x] = 1;
                }
            }
        }
        for (int pass = 0; pass < 2; pass++) {
            std::vector<uint8_t> next = have;
            for (int y = 0; y < td.h; y++) {
                for (int x = 0; x < td.w; x++) {
                    if (have[size_t(y) * td.w + x]) continue;
                    int sum[3] = { 0, 0, 0 }, n = 0;
                    for (int dy = -1; dy <= 1; dy++) for (int dx = -1; dx <= 1; dx++) {
                        int px = x + dx, py = y + dy;
                        if (px < 0 || py < 0 || px >= td.w || py >= td.h || !have[size_t(py) * td.w + px]) continue;
                        for (int q = 0; q < 3; q++) sum[q] += td.rgb[size_t(py) * td.w + px][q];
                        n++;
                    }
                    if (n) {
                        for (int q = 0; q < 3; q++) td.rgb[size_t(y) * td.w + x][q] = uint8_t(sum[q] / n);
                        next[size_t(y) * td.w + x] = 1;
                    }
                }
            }
            have.swap(next);
        }
        for (size_t i = 0; i < have.size(); i++) {
            if (!have[i]) td.rgb[i] = { 0, 0, 0 };
        }
        // Levels: night and indoor tracks are dark from above; brighten (at most 1.6x) so the brightest 3% reach about 225.
        std::vector<int> lum;
        for (size_t i = 0; i < have.size(); i++) {
            if (have[i]) lum.push_back(td.rgb[i][0] * 3 + td.rgb[i][1] * 6 + td.rgb[i][2]);
        }
        float gain = 1.0f;
        if (!lum.empty()) {
            auto nth = lum.begin() + lum.size() * 97 / 100;
            std::nth_element(lum.begin(), nth, lum.end());
            gain = std::clamp(225.0f * 10 / std::max(1, *nth), 1.0f, 1.6f);
        }
        // Night tracks stay dark after that; lift their mid-tones so the median reaches about 60.
        {
            std::vector<int> mid;
            for (size_t i = 0; i < have.size(); i++) {
                if (have[i]) mid.push_back(int(td.rgb[i][0] * 3 + td.rgb[i][1] * 6 + td.rgb[i][2]) / 10);
            }
            if (!mid.empty()) {
                auto nth = mid.begin() + mid.size() / 2;
                std::nth_element(mid.begin(), nth, mid.end());
                float median = std::max(8, *nth) / 255.0f;
                if (median < 60 / 255.0f) {
                    float g = std::clamp(std::log(60 / 255.0f) / std::log(median), 0.8f, 1.0f);
                    for (size_t i = 0; i < have.size(); i++) {
                        if (!have[i]) continue;
                        for (int q = 0; q < 3; q++) td.rgb[i][q] = uint8_t(std::lround(255 * std::pow(td.rgb[i][q] / 255.0f, g)));
                    }
                }
            }
        }
        // Open ground where the track has no geometry: the track's average colour, darker, with a little noise.
        float average[3] = { 0, 0, 0 };
        size_t count = 0;
        for (size_t i = 0; i < have.size(); i++) {
            if (!have[i]) continue;
            for (int q = 0; q < 3; q++) {
                td.rgb[i][q] = uint8_t(std::min(255.0f, td.rgb[i][q] * gain));
                average[q] += td.rgb[i][q];
            }
            count++;
        }
        for (int q = 0; q < 3; q++) average[q] = count ? average[q] / count * 0.7f : 80.0f;
        float average_lum = (average[0] * 3 + average[1] * 6 + average[2]) / 10;
        if (average_lum < 60) {
            for (int q = 0; q < 3; q++) average[q] += 60 - average_lum;
        }
        uint32_t noise = 12345;
        for (size_t i = 0; i < have.size(); i++) {
            if (have[i]) continue;
            noise = noise * 1103515245u + 12345u;
            float n = 0.92f + 0.16f * ((noise >> 16) & 0xFF) / 255.0f;
            for (int q = 0; q < 3; q++) td.rgb[i][q] = uint8_t(std::clamp(average[q] * n, 0.0f, 255.0f));
        }
        return true;
    }

    // ---------------------------------------------------------------------------------------------------------------
    // Diorama: the track's own geometry, shrunk

    // Sizes in model units (the stock dioramas are 500-640 units from the centre to their long side).
    constexpr float diorama_half_size = 560.0f;  // half the longer side of the base
    constexpr float route_half_width = 4.5f;
    constexpr float route_lift = 2.0f;
    constexpr float base_depth = 45.0f;          // wooden skirt below the base's top
    constexpr float point_spacing = 8.0f;
    constexpr int ground_tiles = 4;              // top-down render for the base's colour
    // Shapes (connected parts of an object) smaller than this are left out: poles, lamps, signs, cones, clutter.
    // If a track's miniature comes out over the budget, it is built again with the next, stricter limits.
    constexpr size_t track_budget = 800 * 1024;  // bytes of texels, vertices and lists per track
    struct Detail {
        float shape_size;      // world units, largest side of a shape's bounding box
        float shape_footprint; // world units, largest horizontal side
        float edge;            // model units: triangles whose longest edge is shorter are left out
    };
    constexpr Detail detail_levels[] = { { 60, 20, 1.0f }, { 90, 28, 1.5f }, { 130, 36, 2.0f }, { 180, 45, 2.5f },
                                         { 250, 60, 3.0f } };
    constexpr uint32_t levels_dropped = 2;        // finest mip levels of the track's textures left out
    constexpr float thin_width = 8.0f;           // world units: shapes whose area / length is less are dropped
    constexpr float branch_min_offset = 150.0f;  // a branch is drawn if it leaves the spine by this much (world units)
    constexpr uint8_t route_red[3] = { 196, 0, 0 };

    // G_SETOTHERMODE_H(TEXTLUT = RGBA16) and G_SETBLENDCOLOR as Rush 2 keeps them (see track2049_convert.cpp).
    constexpr uint32_t tlut_rgba16[2] = { 0xE3001001, 0x00008000 };
    constexpr uint32_t blend_rush2[2] = { 0xF9000000, 0x00000010 };

    struct Vertex {
        float x, y, z;
        float s, t;     // texels
        uint8_t r, g, b;
    };

    // CI8 64x32 texture with an RGBA5551 palette, loaded as the stock dioramas load theirs.
    struct Texture {
        std::vector<uint8_t> texels;    // 2048, row-major, row 0 = t 0
        std::array<uint16_t, 256> palette{};
        bool wrap = false;              // wrap (wood) or clamp (print tiles)
    };

    struct Group {
        int texture = -1;               // index into the textures, or -1 for shade only
        std::vector<Vertex> vertices;
        std::vector<std::array<int, 3>> triangles;

        int add(const Vertex& v) {
            vertices.push_back(v);
            return int(vertices.size() - 1);
        }
        // Both windings, so the face shows whatever the geometry mode's culling and mirror state.
        void tri(int a, int b, int c) {
            triangles.push_back({ a, b, c });
            triangles.push_back({ a, c, b });
        }
        void quad(int a, int b, int c, int d) {
            tri(a, b, c);
            tri(a, c, d);
        }
    };

    uint16_t rgba5551(int r, int g, int b, int a) {
        return uint16_t(((r >> 3) << 11) | ((g >> 3) << 6) | ((b >> 3) << 1) | (a ? 1 : 0));
    }

    // Median cut to at most 256 colours.
    void quantize(const std::vector<std::array<uint8_t, 3>>& px, Texture& tex) {
        std::vector<std::vector<int>> boxes(1);
        for (int i = 0; i < int(px.size()); i++) boxes[0].push_back(i);
        auto range = [&](const std::vector<int>& box, int& axis) {
            int best = -1;
            for (int q = 0; q < 3; q++) {
                int lo = 255, hi = 0;
                for (int i : box) { lo = std::min<int>(lo, px[i][q] >> 3); hi = std::max<int>(hi, px[i][q] >> 3); }
                if (hi - lo > best) { best = hi - lo; axis = q; }
            }
            return best;
        };
        struct Split { float score; int axis; };
        std::vector<Split> splits;
        auto measure = [&](const std::vector<int>& box) {
            int axis = 0;
            int r = range(box, axis);
            return Split{ r > 0 ? r * std::sqrt(float(box.size())) : 0.0f, axis };
        };
        splits.push_back(measure(boxes[0]));
        while (boxes.size() < 256) {
            int pick = -1;
            float score = 0;
            for (int b = 0; b < int(boxes.size()); b++) {
                if (splits[b].score > score) { score = splits[b].score; pick = b; }
            }
            if (pick < 0) break;
            auto& box = boxes[pick];
            int axis = splits[pick].axis;
            std::nth_element(box.begin(), box.begin() + box.size() / 2, box.end(),
                             [&](int a, int b) { return px[a][axis] < px[b][axis]; });
            std::vector<int> upper(box.begin() + box.size() / 2, box.end());
            box.resize(box.size() / 2);
            splits[pick] = measure(box);
            splits.push_back(measure(upper));
            boxes.push_back(std::move(upper));
        }
        tex.texels.assign(px.size(), 0);
        for (size_t b = 0; b < boxes.size(); b++) {
            int sum[3] = { 0, 0, 0 };
            for (int i : boxes[b]) {
                for (int q = 0; q < 3; q++) sum[q] += px[i][q];
                tex.texels[i] = uint8_t(b);
            }
            int n = std::max<int>(1, int(boxes[b].size()));
            tex.palette[b] = rgba5551(sum[0] / n, sum[1] / n, sum[2] / n, 1);
        }
    }

    // A wooden board texture for the base's sides, grain along s.
    Texture wood_texture() {
        std::vector<std::array<uint8_t, 3>> px(tile_w * tile_h);
        for (int y = 0; y < tile_h; y++) {
            for (int x = 0; x < tile_w; x++) {
                float grain = std::sin(y * 0.9f + std::sin(x * 0.11f) * 1.6f + std::sin(x * 0.37f + y) * 0.35f);
                float plank = (y % 16 == 0) ? 0.72f : 1.0f;
                float v = (0.84f + 0.16f * grain) * plank;
                px[y * tile_w + x] = { uint8_t(std::min(255.0f, 150 * v)), uint8_t(std::min(255.0f, 96 * v)), uint8_t(std::min(255.0f, 44 * v)) };
            }
        }
        Texture t;
        quantize(px, t);
        t.wrap = true;
        return t;
    }

    // One track's diorama, in four parts placed by the container layout. Pointer words hold offsets within their
    // target part; the fix-ups add the part's position in the container.
    enum PartId { texels_part, vertices_part, list_part, loads_part, part_count };
    struct Fixup {
        int in;         // part holding the word
        uint32_t at;    // its offset there
        int to;         // part the word points into
    };
    struct TrackModel {
        Bytes parts[part_count];        // texels and palettes, vertices, the model list, texture-load lists
        std::vector<Fixup> fixups;
        float radius = 0;               // largest vertex distance from the origin, model units
        size_t vertices = 0, triangles = 0, objects = 0, objects_drawn = 0, shapes_dropped = 0, faces_down = 0, specks = 0,
               translucent_dropped = 0;
        int detail = 0;                 // index into detail_levels
        TopDown print;                  // the top-down render on the base (also for previews)
    };


    // A placed 2049 object: its LOD 0 list and its placement.
    struct Placed {
        uint32_t list = 0;
        float m[9] = {}, pos[3] = {};
        std::vector<uint8_t> tri_kept;  // per triangle in list order (plan)
        std::set<uint32_t> targets;     // conditional ops' targets (plan)
    };

    // Builds one track's miniature: the placed objects' lists copied with their vertices baked to model space and
    // their texture loads and texels carried over, plus the base, the print on it and the route.
    //
    // 2049 track sections' lists (geometry.md §4) are contiguous, end at their first G_ENDDL, and hold only vertex
    // loads, calls of TXLD texture-load lists, conditional ops into themselves, triangles and RDP state. They are
    // copied command by command into one model list (their G_ENDDLs dropped), which the loader's relocator walks in
    // full, so every vertex pointer is rebased and the mirror conditionals are resolved at load as in a race. The
    // texture-load lists go to the container's texture-load range ([7]..[8]), whose SETTIMGs the loader rebases.
    class Miniature {
    public:
        Miniature(const Bytes& geometry, TrackModel& model) : track(model), g(geometry) {
            auto ch = chunks49(g);
            imag = ch["IMAG"].offset; imag_size = ch["IMAG"].size;
            txld = ch["TXLD"].offset; txld_size = ch["TXLD"].size;
            ok = ch["IMAG"].found && ch["TXLD"].found && ch["OBHD"].found && imag + imag_size <= g.size() &&
                 txld + txld_size <= g.size();
        }

        TrackModel& track;
        bool ok = false;
        float centre[3] = { 0, 0, 0 }, scale = 1;   // model = (world - centre) * scale
        float crop[4] = { 0, 0, 0, 0 };              // world x0, x1, z0, z1
        Detail detail = detail_levels[0];
        std::vector<std::pair<float, float>> ground; // kept triangles: lowest model y, horizontal area
        uint32_t alpha_compare = 0;                  // last G_SETALPHACOMPARE the objects set
        bool blend_set = false;                      // an object set the blend colour

        // Distance field to the route (spine and branches), in cells of route_cell world units over the crop.
        static constexpr float route_cell = 25.0f;
        int field_w = 0, field_h = 0;
        std::vector<float> field;
        void set_route(const std::vector<Point>& spine, const std::vector<std::vector<Point>>& branches) {
            field_w = int((crop[1] - crop[0]) / route_cell) + 1;
            field_h = int((crop[3] - crop[2]) / route_cell) + 1;
            field.assign(size_t(field_w) * size_t(field_h), 1e9f);
            auto mark = [&](const std::vector<Point>& pts) {
                for (size_t i = 0; i + 1 < pts.size(); i++) {
                    const Point& a = pts[i];
                    const Point& b = pts[i + 1];
                    int steps = std::max(1, int(std::hypot(b.x - a.x, b.z - a.z) / (route_cell * 0.5f)));
                    for (int st = 0; st <= steps; st++) {
                        float f = float(st) / steps;
                        int x = int((a.x + (b.x - a.x) * f - crop[0]) / route_cell), z = int((a.z + (b.z - a.z) * f - crop[2]) / route_cell);
                        if (x >= 0 && z >= 0 && x < field_w && z < field_h) field[size_t(z) * size_t(field_w) + size_t(x)] = 0;
                    }
                }
            };
            mark(spine);
            for (const auto& b : branches) mark(b);
            // Chamfer distance transform, two passes.
            const float d1 = route_cell, d2 = route_cell * 1.4142f;
            auto at = [&](int x, int z) -> float& { return field[size_t(z) * size_t(field_w) + size_t(x)]; };
            for (int z = 0; z < field_h; z++) {
                for (int x = 0; x < field_w; x++) {
                    float& v = at(x, z);
                    if (x > 0) v = std::min(v, at(x - 1, z) + d1);
                    if (z > 0) v = std::min(v, at(x, z - 1) + d1);
                    if (x > 0 && z > 0) v = std::min(v, at(x - 1, z - 1) + d2);
                    if (x + 1 < field_w && z > 0) v = std::min(v, at(x + 1, z - 1) + d2);
                }
            }
            for (int z = field_h - 1; z >= 0; z--) {
                for (int x = field_w - 1; x >= 0; x--) {
                    float& v = at(x, z);
                    if (x + 1 < field_w) v = std::min(v, at(x + 1, z) + d1);
                    if (z + 1 < field_h) v = std::min(v, at(x, z + 1) + d1);
                    if (x + 1 < field_w && z + 1 < field_h) v = std::min(v, at(x + 1, z + 1) + d2);
                    if (x > 0 && z + 1 < field_h) v = std::min(v, at(x - 1, z + 1) + d2);
                }
            }
        }
        float route_distance(float x, float z) const {
            int cx = int((x - crop[0]) / route_cell), cz = int((z - crop[2]) / route_cell);
            if (cx < 0 || cz < 0 || cx >= field_w || cz >= field_h) return 1e9f;
            return field[size_t(cz) * size_t(field_w) + size_t(cx)];
        }

        void world(const Placed& p, uint32_t vertex, float out[3]) const {
            float v[3] = { int16_t(be16(g, vertex)) * vertex_scale, int16_t(be16(g, vertex + 2)) * vertex_scale,
                           int16_t(be16(g, vertex + 4)) * vertex_scale };
            for (int j = 0; j < 3; j++) out[j] = p.pos[j] + v[0] * p.m[j] + v[1] * p.m[3 + j] + v[2] * p.m[6 + j];
        }

        static float triangle_area(const float* w) {
            float u[3] = { w[3] - w[0], w[4] - w[1], w[5] - w[2] };
            float v[3] = { w[6] - w[0], w[7] - w[1], w[8] - w[2] };
            float n[3] = { u[1] * v[2] - u[2] * v[1], u[2] * v[0] - u[0] * v[2], u[0] * v[1] - u[1] * v[0] };
            return 0.5f * std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
        }

        // Pass 1: checks the list, splits its triangles into shapes (connected triangles) and decides what is kept.
        // Returns false if the list holds something the miniature can't carry over; the object is then left out.
        bool plan(Placed& p) {
            struct Slot { float w[3] = { 0, 0, 0 }; int node = -1; };
            Slot slots[32];
            std::vector<int> parent;
            std::vector<std::array<float, 6>> box;      // per node: min xyz, max xyz
            std::unordered_map<uint64_t, int> nodes;    // rounded world position -> node
            auto find = [&](int a) {
                while (parent[size_t(a)] != a) a = parent[size_t(a)] = parent[size_t(parent[size_t(a)])];
                return a;
            };
            struct Tri { int node[3]; float w[9]; bool translucent; };
            std::vector<Tri> tris;
            std::vector<float> area;                    // per node: area of the triangles joined to it
            uint32_t render_mode = 0;
            for (uint32_t pc = p.list;; pc += 8) {
                if (pc + 8 > g.size() || pc - p.list > 0x100000) return false;
                uint32_t w0 = be32(g, pc), w1 = be32(g, pc + 4);
                uint8_t op = uint8_t(w0 >> 24);
                if (op == 0xDF) break;
                switch (op) {
                case 0x01: {
                    uint32_t n = (w0 >> 12) & 0xFF, end = (w0 >> 1) & 0x7F, addr = w1 & 0xFFFFFF;
                    if (n == 0 || n > end || end > 32 || addr + n * 16 > g.size()) return false;
                    for (uint32_t i = 0; i < n; i++) {
                        Slot& s = slots[end - n + i];
                        world(p, addr + i * 16, s.w);
                        uint64_t key = 0;
                        for (int j = 0; j < 3; j++) key = (key << 21) | (uint64_t(std::lround(s.w[j]) + 0x100000) & 0x1FFFFF);
                        auto it = nodes.find(key);
                        if (it == nodes.end()) {
                            it = nodes.emplace(key, int(parent.size())).first;
                            parent.push_back(int(parent.size()));
                            box.push_back({ s.w[0], s.w[1], s.w[2], s.w[0], s.w[1], s.w[2] });
                            area.push_back(0);
                        }
                        s.node = it->second;
                    }
                    break;
                }
                case 0x05: case 0x06: case 0x07:
                    for (int half = 0; half < (op == 0x05 ? 1 : 2); half++) {
                        uint32_t w = half ? w1 : w0;
                        Tri t;
                        for (int q = 0; q < 3; q++) {
                            int slot = int((w >> (16 - 8 * q)) & 0xFF) / 2;
                            if (slot >= 32 || slots[slot].node < 0) return false;
                            t.node[q] = slots[slot].node;
                            for (int j = 0; j < 3; j++) t.w[q * 3 + j] = slots[slot].w[j];
                        }
                        t.translucent = (render_mode & 0x4000) && !(render_mode & 0x20); // FORCE_BL, no Z_UPD
                        int a = find(t.node[0]);
                        area[size_t(a)] += triangle_area(t.w);
                        for (int q = 1; q < 3; q++) {
                            int b = find(t.node[q]);
                            if (a == b) continue;
                            parent[size_t(b)] = a;
                            area[size_t(a)] += area[size_t(b)];
                            for (int j = 0; j < 3; j++) {
                                box[size_t(a)][j] = std::min(box[size_t(a)][j], box[size_t(b)][j]);
                                box[size_t(a)][3 + j] = std::max(box[size_t(a)][3 + j], box[size_t(b)][3 + j]);
                            }
                        }
                        tris.push_back(t);
                    }
                    break;
                case 0xDE:
                    if (((w0 >> 16) & 0xFF) != 0 || !walk_loads(w1 & 0xFFFFFF, nullptr)) return false;
                    break;
                case 0xE2:
                    if (w0 == 0xE200001C) render_mode = w1;
                    break;
                case 0xE0:
                    if (((w0 >> 16) & 0xFF) == 1) {
                        if ((w1 & 0xFFFFFF) <= pc || (w1 & 0xFFFFFF) >= g.size()) return false;
                        p.targets.insert(w1 & 0xFFFFFF);
                    }
                    break;
                // Pointers the relocator would rebase or follow: none in 2049 track sections.
                case 0xDA: case 0xDC: case 0xDD: case 0xE1: case 0xFD: case 0xFE: case 0xFF: case 0x04:
                    return false;
                default:
                    break;
                }
            }
            p.tri_kept.assign(tris.size(), 0);
            std::unordered_map<int, bool> shape_kept;
            for (size_t i = 0; i < tris.size(); i++) {
                const Tri& t = tris[i];
                int root = find(t.node[0]);
                auto it = shape_kept.find(root);
                if (it == shape_kept.end()) {
                    const auto& b = box[size_t(root)];
                    float dx = b[3] - b[0], dy = b[4] - b[1], dz = b[5] - b[2];
                    auto passes = [&](const Detail& l) {
                        return std::max({ dx, dy, dz }) >= l.shape_size && std::max(dx, dz) >= l.shape_footprint;
                    };
                    // Near the route only the finest level's limits apply, so the road and what lines it stay whole.
                    bool near = route_distance((b[0] + b[3]) * 0.5f, (b[2] + b[5]) * 0.5f) < 80.0f + 0.5f * std::max(dx, dz);
                    bool keep = passes(detail) || (near && passes(detail_levels[0]));
                    // Wires, cables, poles and thin rails: long shapes with little surface for their length.
                    if (area[size_t(root)] < thin_width * std::max({ dx, dy, dz })) keep = false;
                    track.shapes_dropped += !keep;
                    it = shape_kept.emplace(root, keep).first;
                }
                float cx = (t.w[0] + t.w[3] + t.w[6]) / 3, cz = (t.w[2] + t.w[5] + t.w[8]) / 3;
                if (!it->second || cx < crop[0] || cx > crop[1] || cz < crop[2] || cz > crop[3]) continue;
                // Faces whose front side looks down (ceilings, undersides) can't be seen from above, and the smallest
                // triangles would be specks.
                {
                    float edge = 0;
                    for (int q = 0; q < 3; q++) {
                        const float* a = &t.w[q * 3];
                        const float* b = &t.w[((q + 1) % 3) * 3];
                        edge = std::max(edge, std::sqrt((a[0] - b[0]) * (a[0] - b[0]) + (a[1] - b[1]) * (a[1] - b[1]) + (a[2] - b[2]) * (a[2] - b[2])));
                    }
                    if (edge * scale < detail.edge) { track.specks++; continue; }
                    float u[3] = { t.w[3] - t.w[0], t.w[4] - t.w[1], t.w[5] - t.w[2] };
                    float v[3] = { t.w[6] - t.w[0], t.w[7] - t.w[1], t.w[8] - t.w[2] };
                    float n[3] = { u[1] * v[2] - u[2] * v[1], u[2] * v[0] - u[0] * v[2], u[0] * v[1] - u[1] * v[0] };
                    float len = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
                    if (len > 0 && n[1] > 0.3f * len) { track.faces_down++; continue; }
                    // Translucent panels that aren't flat on the ground (glass, light shafts, haze cards) read as stray
                    // grey sheets at this size; translucent water stays.
                    if (t.translucent && len > 0 && std::fabs(n[1]) < 0.7f * len) { track.translucent_dropped++; continue; }
                }
                p.tri_kept[i] = 1;
                float footprint = std::fabs((t.w[3] - t.w[0]) * (t.w[8] - t.w[2]) - (t.w[6] - t.w[0]) * (t.w[5] - t.w[2])) * 0.5f;
                float lowest = std::min({ t.w[1], t.w[4], t.w[7] });
                ground.push_back({ (lowest - centre[1]) * scale, footprint });
            }
            return true;
        }

        // Pass 2: appends the object's kept commands to the model list. Vertex loads and triangles are rebuilt: each
        // run of kept triangles between two state commands becomes fresh G_VTX batches holding only the vertices
        // they use, with vertices that come out the same at the miniature's scale merged.
        void emit(const Placed& p) {
            Bytes& list = track.parts[list_part];
            std::unordered_map<uint32_t, uint32_t> moved;              // 2049 command -> list offset
            std::vector<std::pair<uint32_t, uint32_t>> conditionals;   // list offset of the target word, 2049 target
            std::unordered_map<uint32_t, std::array<uint8_t, 16>> baked; // 2049 vertex -> miniature vertex
            std::vector<std::array<uint32_t, 3>> run;                  // kept triangles, as 2049 vertex addresses
            uint32_t slots[32] = {};
            size_t tri = 0;

            auto bake = [&](uint32_t a) -> const std::array<uint8_t, 16>& {
                auto it = baked.find(a);
                if (it != baked.end()) return it->second;
                float w[3], v[3];
                world(p, a, w);
                for (int j = 0; j < 3; j++) v[j] = (w[j] - centre[j]) * scale;
                v[0] = std::clamp(v[0], (crop[0] - centre[0]) * scale, (crop[1] - centre[0]) * scale);
                v[2] = std::clamp(v[2], (crop[2] - centre[2]) * scale, (crop[3] - centre[2]) * scale);
                std::array<uint8_t, 16> b{};
                for (int j = 0; j < 3; j++) {
                    uint16_t c = uint16_t(int16_t(std::clamp(std::lround(v[j]), -32767L, 32767L)));
                    b[size_t(j * 2)] = uint8_t(c >> 8); b[size_t(j * 2 + 1)] = uint8_t(c);
                }
                uint16_t flags = uint16_t(0x8000 | (be16(g, a + 6) & 0x01FF));
                b[6] = uint8_t(flags >> 8); b[7] = uint8_t(flags);
                memcpy(&b[8], &g[a + 8], 8);
                return baked.emplace(a, b).first->second;
            };
            auto flush = [&]() {
                std::vector<std::array<uint8_t, 16>> batch;
                std::map<std::array<uint8_t, 16>, int> local;
                std::vector<std::array<int, 3>> tris;
                auto out = [&]() {
                    if (batch.empty()) return;
                    uint32_t n = uint32_t(batch.size());
                    command(0x01000000 | (n << 12) | (n << 1), uint32_t(track.parts[vertices_part].size()), vertices_part);
                    for (const auto& b : batch) add_raw_vertex(b);
                    auto word = [](const std::array<int, 3>& t) {
                        return (uint32_t(t[0] * 2) << 16) | (uint32_t(t[1] * 2) << 8) | uint32_t(t[2] * 2);
                    };
                    for (size_t t = 0; t < tris.size(); t += 2) {
                        if (t + 1 < tris.size()) command(0x06000000 | word(tris[t]), word(tris[t + 1]));
                        else command(0x05000000 | word(tris[t]), 0);
                    }
                    track.triangles += tris.size();
                    batch.clear(); local.clear(); tris.clear();
                };
                for (const auto& t : run) {
                    std::array<uint8_t, 16> v[3] = { bake(t[0]), bake(t[1]), bake(t[2]) };
                    // Triangles whose corners merged at this scale draw nothing.
                    if (!memcmp(v[0].data(), v[1].data(), 6) || !memcmp(v[1].data(), v[2].data(), 6) ||
                        !memcmp(v[0].data(), v[2].data(), 6)) continue;
                    int added = 0;
                    for (const auto& x : v) added += !local.contains(x);
                    if (batch.size() + size_t(added) > 32) out();
                    std::array<int, 3> lt;
                    for (int q = 0; q < 3; q++) {
                        auto it = local.find(v[q]);
                        if (it == local.end()) {
                            it = local.emplace(v[q], int(batch.size())).first;
                            batch.push_back(v[q]);
                        }
                        lt[size_t(q)] = it->second;
                    }
                    tris.push_back(lt);
                }
                out();
                run.clear();
            };

            uint32_t pc = p.list;
            for (;; pc += 8) {
                uint32_t w0 = be32(g, pc), w1 = be32(g, pc + 4);
                uint8_t op = uint8_t(w0 >> 24);
                bool geometry = op == 0x01 || op == 0x05 || op == 0x06 || op == 0x07;
                if (!geometry || p.targets.contains(pc)) flush();
                moved[pc] = uint32_t(list.size());
                if (op == 0xDF) break;
                switch (op) {
                case 0x01: {
                    uint32_t n = (w0 >> 12) & 0xFF, end = (w0 >> 1) & 0x7F, addr = w1 & 0xFFFFFF;
                    for (uint32_t i = 0; i < n; i++) slots[end - n + i] = addr + i * 16;
                    break;
                }
                case 0x05: case 0x06: case 0x07:
                    for (int half = 0; half < (op == 0x05 ? 1 : 2); half++) {
                        uint32_t w = half ? w1 : w0;
                        if (!p.tri_kept[tri++]) continue;
                        run.push_back({ slots[((w >> 16) & 0xFF) / 2], slots[((w >> 8) & 0xFF) / 2], slots[(w & 0xFF) / 2] });
                    }
                    break;
                case 0xDE:
                    command(w0, copy_loads(w1 & 0xFFFFFF), loads_part);
                    break;
                case 0xE0:
                    if (((w0 >> 16) & 0xFF) == 1) {
                        conditionals.push_back({ uint32_t(list.size() + 4), w1 & 0xFFFFFF });
                        command(w0, 0, list_part);
                    }
                    else command(w0, w1);
                    break;
                case 0xE2:
                    if (w0 == 0xE2001E01) alpha_compare = w1;
                    command(w0, w1);
                    break;
                // No mipmapping: the miniature is far smaller on screen than the track in a race, so the RDP would pick
                // the coarsest levels and flatten every texture to its average colour. Texture LOD stays off and the
                // mip combiner (TEXEL1 to TEXEL0 by LOD fraction, times shade) becomes TEXEL0 times shade, so tile 0
                // (the finest level kept) is drawn, as the stock dioramas draw their textures.
                case 0xE3:
                    command(w0, w0 == 0xE3000F00 ? 0 : w1);
                    break;
                case 0xFC:
                    if (w0 == 0xFC26A004 && w1 == 0x1FFC93F8) command(0xFC127FFF, 0xFFFFF238);
                    else command(w0, w1);
                    break;
                case 0xF9:
                    blend_set = true;
                    command(w0, w1);
                    break;
                default:
                    command(w0, w1);
                    break;
                }
            }
            // A conditional whose target is the G_ENDDL now skips to whatever follows the object.
            for (auto [at, target] : conditionals) {
                auto it = moved.find(target);
                put32(list, at, it != moved.end() ? it->second : moved[pc]);
            }
        }

        void add_raw_vertex(const std::array<uint8_t, 16>& b) {
            Bytes& out = track.parts[vertices_part];
            out.insert(out.end(), b.begin(), b.end());
            float v[3];
            for (int j = 0; j < 3; j++) v[j] = float(int16_t((b[size_t(j * 2)] << 8) | b[size_t(j * 2 + 1)]));
            track.radius = std::max(track.radius, std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]));
            track.vertices++;
        }

        // Texels of the copied texture-load lists, then their SETTIMGs pointed at them. Call once, after emit.
        void finish_texels() {
            Bytes& texels = track.parts[texels_part];
            std::sort(ranges.begin(), ranges.end());
            std::vector<std::array<uint32_t, 3>> merged;            // IMAG-relative lo, hi; offset in texels
            for (auto [lo, hi] : ranges) {
                lo &= ~7u;
                hi = std::min((hi + 7) & ~7u, uint32_t(g.size() - imag));
                if (!merged.empty() && lo <= merged.back()[1]) {
                    merged.back()[1] = std::max(merged.back()[1], hi);
                    continue;
                }
                merged.push_back({ lo, hi, 0 });
            }
            for (auto& r : merged) {
                align(texels, 8);
                r[2] = uint32_t(texels.size());
                texels.insert(texels.end(), g.begin() + imag + r[0], g.begin() + imag + r[1]);
            }
            Bytes& loads = track.parts[loads_part];
            for (auto [at, address] : settimg) {
                auto it = std::upper_bound(merged.begin(), merged.end(), address,
                                           [](uint32_t a, const std::array<uint32_t, 3>& r) { return a < r[0]; });
                uint32_t to = it == merged.begin() ? 0 : (*(it - 1))[2] + address - (*(it - 1))[0];
                put32(loads, at, (be32(loads, at) & 0xFF000000) | to);
                track.fixups.push_back({ loads_part, at, texels_part });
            }
        }

        void command(uint32_t w0, uint32_t w1, int points_to = -1) {
            Bytes& list = track.parts[list_part];
            if (points_to >= 0) track.fixups.push_back({ list_part, uint32_t(list.size() + 4), points_to });
            push32(list, w0);
            push32(list, w1);
        }

        // Every vertex is chained for the loader's mirror pass (bit 15 of +6); the last is unchained at the end.
        void add_model_vertex(const float v[3], uint16_t flags, uint16_t s, uint16_t t, const uint8_t* rgba) {
            Bytes& out = track.parts[vertices_part];
            for (int j = 0; j < 3; j++) push16(out, uint16_t(int16_t(std::clamp(std::lround(v[j]), -32767L, 32767L))));
            push16(out, uint16_t(0x8000 | (flags & 0x01FF)));
            push16(out, s);
            push16(out, t);
            out.insert(out.end(), rgba, rgba + 4);
            track.radius = std::max(track.radius, std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]));
            track.vertices++;
        }

    private:
        const Bytes& g;
        uint32_t imag = 0, imag_size = 0, txld = 0, txld_size = 0;
        std::map<uint32_t, uint32_t> loads_copied;                  // TXLD list -> offset in the loads part
        std::vector<std::pair<uint32_t, uint32_t>> ranges;          // IMAG-relative [lo, hi) read by the loads
        std::vector<std::pair<uint32_t, uint32_t>> settimg;         // loads part offset of a SETTIMG's address word,
                                                                    // IMAG-relative address

        // Walks a texture-load list in d from list (2049's TXLD or a rewritten copy): it must end with G_ENDDL before
        // limit, call nothing and read only IMAG. With out, also adds the IMAG ranges its SETTIMGs and loads read.
        bool walk_loads(const Bytes& d, uint32_t list, uint32_t limit,
                        std::vector<std::pair<uint32_t, uint32_t>>* out) const {
            if (list & 7) return false;
            uint32_t addr = 0, siz = 0, width = 1;
            for (uint32_t pc = list; pc + 8 <= limit; pc += 8) {
                uint32_t w0 = be32(d, pc), w1 = be32(d, pc + 4);
                uint32_t lo = 0, hi = 0, bits = 4u << siz;
                switch (w0 >> 24) {
                case 0xDF: return true;
                case 0xFD:
                    addr = w1 & 0xFFFFFF; siz = (w0 >> 19) & 3; width = (w0 & 0xFFF) + 1;
                    lo = addr; hi = addr + 8;
                    break;
                case 0xF3: {
                    uint32_t uls = (w0 >> 12) & 0xFFF, lrs = (w1 >> 12) & 0xFFF;
                    if (lrs < uls) return false;
                    lo = addr + uls * bits / 8; hi = addr + ((lrs + 1) * bits + 7) / 8;
                    break;
                }
                case 0xF0: {
                    uint32_t first = ((w0 >> 12) & 0xFFF) >> 2, last = ((w1 >> 12) & 0xFFF) >> 2;
                    if (last < first) return false;
                    lo = addr + first * 2; hi = addr + (last + 1) * 2;
                    break;
                }
                case 0xF4: {
                    uint32_t uls = ((w0 >> 12) & 0xFFF) >> 2, ult = (w0 & 0xFFF) >> 2;
                    uint32_t lrs = ((w1 >> 12) & 0xFFF) >> 2, lrt = (w1 & 0xFFF) >> 2;
                    if (lrs < uls || lrt < ult) return false;
                    lo = addr + (ult * width + uls) * bits / 8; hi = addr + ((lrt * width + lrs + 1) * bits + 7) / 8;
                    break;
                }
                case 0x01: case 0xDA: case 0xDC: case 0xDD: case 0xDE: case 0xE1: case 0xFE: case 0xFF: case 0x04:
                    return false;
                default:
                    continue;
                }
                if (hi > imag_size) return false;
                if (out) out->push_back({ lo, hi });
            }
            return false;
        }
        bool walk_loads(uint32_t list, std::vector<std::pair<uint32_t, uint32_t>>* out) const {
            return list >= txld && list < txld + txld_size && walk_loads(g, list, txld + txld_size, out);
        }

        // Leaves out the most detailed mip levels of a texture-load list (at least two levels stay): every 2049 track
        // texture is a mip chain loaded with one LOADBLOCK (dxt 0, so TMEM holds the bytes as stored) into tiles 0..n
        // at increasing TMEM addresses, each tile with its level's shift. Level d becomes tile 0 by loading from its
        // first byte and renumbering the tiles; the shifts keep the vertices' texture coordinates valid. The RDP picks
        // levels from the base coordinates, so the miniature shows its textures d levels coarser, as befits its size.
        Bytes drop_levels(const Bytes& c) const {
            int d7 = -1, load = -1, image = -1;
            uint32_t tmem[8];
            bool seen[8] = {};
            for (size_t i = 0; i + 8 <= c.size(); i += 8) {
                uint32_t w0 = be32(c, i), w1 = be32(c, i + 4);
                switch (w0 >> 24) {
                case 0xD7: d7 = int(i); break;
                case 0xFD: if (image < 0) image = int(i); break;
                case 0xF3: if (load < 0) load = int(i); break;
                case 0xF5: {
                    uint32_t t = (w1 >> 24) & 7;
                    if (t < 7) { tmem[t] = w0 & 0x1FF; seen[t] = true; }
                    break;
                }
                default: break;
                }
            }
            if (d7 < 0 || load < 0 || image < 0 || image > load) return c;
            uint32_t d7w0 = be32(c, size_t(d7)), level = (d7w0 >> 11) & 7;
            uint32_t drop = std::min<uint32_t>(levels_dropped, level >= 1 ? level - 1 : 0);
            uint32_t lw0 = be32(c, size_t(load)), lw1 = be32(c, size_t(load) + 4);
            if (drop == 0 || ((d7w0 >> 8) & 7) != 0 || (lw1 & 0xFFF) != 0 || ((lw0 >> 12) & 0xFFF) != 0 || !seen[drop]) return c;
            uint32_t bits = 4u << ((be32(c, size_t(image)) >> 19) & 3);
            uint32_t skip = tmem[drop] * 8, bytes = (((lw1 >> 12) & 0xFFF) + 1) * bits / 8;
            if (skip >= bytes) return c;
            for (uint32_t t = drop; t < 7; t++) {
                if (seen[t] && tmem[t] < tmem[drop]) return c;
            }
            Bytes out;
            for (size_t i = 0; i + 8 <= c.size(); i += 8) {
                uint32_t w0 = be32(c, i), w1 = be32(c, i + 4);
                uint8_t op = uint8_t(w0 >> 24);
                if (int(i) == image) w1 = (w1 & 0xFF000000) | ((w1 & 0xFFFFFF) + skip);
                else if (int(i) == load) w1 = (w1 & 0xFF000FFF) | ((((bytes - skip) * 8 / bits) - 1) << 12);
                else if (int(i) == d7) w0 = (w0 & ~(7u << 11)) | ((level - drop) << 11);
                else if (op == 0xF5 || op == 0xF2) {
                    uint32_t t = (w1 >> 24) & 7;
                    if (t < 7) {
                        if (t < drop) continue;
                        w1 = (w1 & ~(7u << 24)) | ((t - drop) << 24);
                        if (op == 0xF5) w0 = (w0 & ~0x1FFu) | ((w0 & 0x1FF) - tmem[drop]);
                    }
                }
                push32(out, w0);
                push32(out, w1);
            }
            return out;
        }

        // Copies a TXLD list to the loads part once, with its finest mip levels left out; returns its offset there.
        uint32_t copy_loads(uint32_t list) {
            auto it = loads_copied.find(list);
            if (it != loads_copied.end()) return it->second;
            Bytes original;
            for (uint32_t pc = list;; pc += 8) {
                push32(original, be32(g, pc));
                push32(original, be32(g, pc + 4));
                if (g[pc] == 0xDF) break;
            }
            Bytes c = drop_levels(original);
            if (!walk_loads(c, 0, uint32_t(c.size()), nullptr)) c = original;
            walk_loads(c, 0, uint32_t(c.size()), &ranges);
            Bytes& loads = track.parts[loads_part];
            uint32_t at = uint32_t(loads.size());
            loads_copied[list] = at;
            for (size_t i = 0; i < c.size(); i += 8) {
                if (c[i] == 0xFD) settimg.push_back({ uint32_t(loads.size() + 4), be32(c, i + 4) & 0xFFFFFF });
                loads.insert(loads.end(), c.begin() + long(i), c.begin() + long(i) + 8);
            }
            return at;
        }
    };

    // Appends a group as G_VTX batches of at most 32 vertices and their triangles.
    void emit_group(Miniature& mini, const Group& group) {
        std::vector<int> batch;
        std::map<int, int> local;
        std::vector<std::array<int, 3>> tris;
        auto flush = [&]() {
            if (batch.empty()) return;
            uint32_t n = uint32_t(batch.size());
            mini.command(0x01000000 | (n << 12) | (n << 1), uint32_t(mini.track.parts[vertices_part].size()), vertices_part);
            for (int v : batch) {
                const Vertex& vx = group.vertices[size_t(v)];
                float p[3] = { vx.x, vx.y, vx.z };
                uint8_t rgba[4] = { vx.r, vx.g, vx.b, 0xFF };
                mini.add_model_vertex(p, 0, uint16_t(int16_t(std::lround(vx.s * 32))), uint16_t(int16_t(std::lround(vx.t * 32))), rgba);
            }
            auto word = [](const std::array<int, 3>& tr) {
                return (uint32_t(tr[0] * 2) << 16) | (uint32_t(tr[1] * 2) << 8) | uint32_t(tr[2] * 2);
            };
            for (size_t t = 0; t < tris.size(); t += 2) {
                if (t + 1 < tris.size()) mini.command(0x06000000 | word(tris[t]), word(tris[t + 1]));
                else mini.command(0x05000000 | word(tris[t]), 0);
            }
            mini.track.triangles += tris.size();
            batch.clear();
            local.clear();
            tris.clear();
        };
        for (const auto& t : group.triangles) {
            int added = 0;
            for (int v : t) added += !local.contains(v);
            if (batch.size() + size_t(added) > 32) flush();
            std::array<int, 3> lt;
            for (int q = 0; q < 3; q++) {
                auto it = local.find(t[q]);
                if (it == local.end()) {
                    it = local.emplace(t[q], int(batch.size())).first;
                    batch.push_back(t[q]);
                }
                lt[q] = it->second;
            }
            tris.push_back(lt);
        }
        flush();
    }

    // Inline CI8 64x32 load of a texture in the texels part, as the stock dioramas' texture-load lists do it.
    void emit_texture_load(Miniature& mini, const Texture& t) {
        Bytes& texels = mini.track.parts[texels_part];
        align(texels, 8);
        uint32_t at = uint32_t(texels.size());
        texels.insert(texels.end(), t.texels.begin(), t.texels.end());
        uint32_t palette = uint32_t(texels.size());
        for (uint16_t c : t.palette) push16(texels, c);
        uint32_t mode = t.wrap ? (5u << 14) | (6u << 4) : (2u << 18) | (2u << 8); // wrap 64x32, or clamp
        mini.command(0xFD500000, at, texels_part);            // SETTIMG CI 16-bit
        mini.command(0xF5500000, 0x07000000 | mode);         // SETTILE 7
        mini.command(0xE6000000, 0);                         // LOADSYNC
        mini.command(0xF3000000, 0x073FF100);                // LOADBLOCK 1024 16-bit texels, dxt 64 bytes/row
        mini.command(0xE7000000, 0);                         // PIPESYNC
        mini.command(0xF5481000, mode);                      // SETTILE 0: CI8, 8 words per row, TMEM 0
        mini.command(0xF2000000, 0x000FC07C);                // SETTILESIZE 64x32
        mini.command(0xFD100000, palette, texels_part);      // SETTIMG palette
        mini.command(0xE8000000, 0);                         // TILESYNC
        mini.command(0xF5000100, 0x07000000);                // SETTILE 7 at TMEM 0x100
        mini.command(0xE6000000, 0);                         // LOADSYNC
        mini.command(0xF0000000, 0x073FC000);                // LOADTLUT 256 entries
        mini.command(0xE7000000, 0);                         // PIPESYNC
    }

    // Route ribbon through pts (model space), flat and lifted a little.
    void add_ribbon(Group& route, const std::vector<Point>& pts, bool closed) {
        std::vector<Point> p;
        for (const Point& q : pts) {
            if (p.empty() || std::hypot(q.x - p.back().x, q.z - p.back().z) >= point_spacing) p.push_back(q);
        }
        if (p.size() < 2) return;
        if (closed) p.push_back(p.front());
        int prev_l = -1, prev_r = -1;
        for (size_t i = 0; i < p.size(); i++) {
            const Point& a = p[i == 0 ? (closed ? p.size() - 2 : 0) : i - 1];
            const Point& b = p[i + 1 < p.size() ? i + 1 : (closed ? 1 : i)];
            float dx = b.x - a.x, dz = b.z - a.z;
            float len = std::hypot(dx, dz);
            if (len < 1e-3f) { dx = 1; dz = 0; len = 1; }
            float nx = -dz / len * route_half_width, nz = dx / len * route_half_width;
            float y = p[i].y + route_lift;
            int l = route.add({ p[i].x + nx, y, p[i].z + nz, 0, 0, route_red[0], route_red[1], route_red[2] });
            int r = route.add({ p[i].x - nx, y, p[i].z - nz, 0, 0, route_red[0], route_red[1], route_red[2] });
            if (prev_l >= 0) route.quad(prev_l, l, r, prev_r);
            prev_l = l; prev_r = r;
        }
    }

    // Track k's miniature.
    // Track k's miniature at one detail level, from its geometry and placement files; m.print is already rendered.
    bool build_track_model_at(const Bytes& geometry, const Bytes& placement, int detail, TrackModel& m) {
        const TopDown& td = m.print;
        Miniature mini(geometry, m);
        if (!mini.ok) return false;
        m.detail = detail;
        mini.detail = detail_levels[detail];
        for (int j = 0; j < 4; j++) mini.crop[j] = td.area[j];
        mini.centre[0] = (td.area[0] + td.area[1]) * 0.5f;
        mini.centre[1] = td.base_y;
        mini.centre[2] = (td.area[2] + td.area[3]) * 0.5f;
        mini.scale = diorama_half_size / (std::max(td.area[1] - td.area[0], td.area[3] - td.area[2]) * 0.5f);
        mini.set_route(td.spine, td.branches);
        auto to_model = [&](const Point& q) {
            return Point{ (q.x - mini.centre[0]) * mini.scale, (q.y - mini.centre[1]) * mini.scale, (q.z - mini.centre[2]) * mini.scale };
        };

        // Objects (OBHD, 0x58 bytes: name, radius, kind, LOD count, 4 x {texture, flags, distance, list, vertices}):
        // LOD 0. Most objects have only that; the far LODs of the others are impostor cards of a handful of
        // triangles (e.g. TRACK3L24834: 308 triangles, then 10), which look like stray sheets on the miniature.
        auto ch = chunks49(geometry);
        struct Object { uint32_t list; float radius; };
        std::map<std::string, Object> objects;
        for (uint32_t i = 0; i < ch["OBHD"].size; i++) {
            uint32_t r = ch["OBHD"].offset + i * 0x58;
            if (r + 0x58 > geometry.size()) return false;
            std::string name(reinterpret_cast<const char*>(&geometry[r]), strnlen(reinterpret_cast<const char*>(&geometry[r]), 15));
            int lods = std::clamp<int>(int16_t(be16(geometry, r + 0x16)), 0, 4);
            uint32_t list = lods >= 1 ? be32(geometry, r + 0x18 + 8) : 0;
            if (list != 0 && name != "SKYSKY") objects[name] = { list, as_float(be32(geometry, r + 16)) };
        }

        // Placed objects near the crop: WOBJ records of 0x68 bytes {name[16], f32 m[9], f32 pos[3], ...}, positions
        // in world space (docs/rush2049_research/placement.md).
        if (placement.size() < 8) return false;
        uint32_t dir = be32(placement, 0), dir_count = be32(placement, 4);
        uint32_t wobj = 0, wobj_count = 0;
        for (uint32_t i = 0; i < dir_count && dir + i * 12 + 12 <= placement.size(); i++) {
            if (memcmp(&placement[dir + i * 12], "WOBJ", 4) == 0) {
                wobj = be32(placement, dir + i * 12 + 4);
                wobj_count = be32(placement, dir + i * 12 + 8);
            }
        }
        if (wobj_count == 0 || wobj + size_t(wobj_count) * 0x68 > placement.size()) return false;
        std::vector<Placed> placed;
        for (uint32_t i = 0; i < wobj_count; i++) {
            uint32_t r = wobj + i * 0x68;
            std::string name(reinterpret_cast<const char*>(&placement[r]), strnlen(reinterpret_cast<const char*>(&placement[r]), 15));
            auto it = objects.find(name);
            if (it == objects.end()) continue;
            Placed p;
            p.list = it->second.list;
            for (int j = 0; j < 9; j++) p.m[j] = as_float(be32(placement, r + 0x10 + j * 4));
            for (int j = 0; j < 3; j++) p.pos[j] = as_float(be32(placement, r + 0x34 + j * 4));
            float reach = it->second.radius * 2.0f;
            if (p.pos[0] + reach < mini.crop[0] || p.pos[0] - reach > mini.crop[1] || p.pos[2] + reach < mini.crop[2] ||
                p.pos[2] - reach > mini.crop[3]) continue;
            m.objects++;
            if (mini.plan(p)) placed.push_back(std::move(p));
        }
        m.objects_drawn = placed.size();

        // The base's top: under nearly all of the kept geometry (2% of its area may sink into it), and below the road.
        float base_top = 0;
        {
            std::sort(mini.ground.begin(), mini.ground.end());
            double total = 0, sum = 0;
            for (auto& gr : mini.ground) total += gr.second;
            for (auto& gr : mini.ground) {
                sum += gr.second;
                if (sum >= total * 0.02) { base_top = gr.first; break; }
            }
            base_top = std::min(base_top, -1.0f) - 1.5f;
        }
        const float x0 = (mini.crop[0] - mini.centre[0]) * mini.scale, x1 = (mini.crop[1] - mini.centre[0]) * mini.scale;
        const float z0 = (mini.crop[2] - mini.centre[2]) * mini.scale, z1 = (mini.crop[3] - mini.centre[2]) * mini.scale;

        // Route: the spine and the branches that really leave it (the others are lanes beside it).
        Group route;
        std::vector<Point> spine;
        for (const Point& q : td.spine) spine.push_back(to_model(q));
        add_ribbon(route, spine, td.closed);
        for (const auto& b : td.branches) {
            float offset = 0;
            for (const Point& q : b) {
                float nearest = 1e30f;
                for (const Point& s : td.spine) nearest = std::min(nearest, (q.x - s.x) * (q.x - s.x) + (q.z - s.z) * (q.z - s.z));
                offset = std::max(offset, std::sqrt(nearest));
            }
            if (offset < branch_min_offset) continue;
            std::vector<Point> pts;
            for (const Point& q : b) pts.push_back(to_model(q));
            add_ribbon(route, pts, false);
        }

        // The base's top: the track's average colour from above, darkened, so gaps between the objects read as
        // ground.
        std::vector<Texture> textures;
        std::vector<Group> textured;
        Group top;
        {
            float average[3] = { 0, 0, 0 };
            for (const auto& c : td.rgb) for (int q = 0; q < 3; q++) average[q] += c[q];
            uint8_t c[3];
            for (int q = 0; q < 3; q++) c[q] = uint8_t(std::clamp(average[q] / std::max<size_t>(1, td.rgb.size()) * 0.75f, 30.0f, 160.0f));
            int a = top.add({ x0, base_top, z0, 0, 0, c[0], c[1], c[2] }), b = top.add({ x1, base_top, z0, 0, 0, c[0], c[1], c[2] });
            int d = top.add({ x1, base_top, z1, 0, 0, c[0], c[1], c[2] }), e = top.add({ x0, base_top, z1, 0, 0, c[0], c[1], c[2] });
            top.quad(a, b, d, e);
        }
        // Wooden skirt around the base, shaded per side as if lit from one corner.
        textures.push_back(wood_texture());
        {
            Group skirt;
            skirt.texture = int(textures.size() - 1);
            const float corners[5][2] = { { x0, z0 }, { x1, z0 }, { x1, z1 }, { x0, z1 }, { x0, z0 } };
            const uint8_t shade[4] = { 150, 205, 255, 185 };
            const float bottom = base_top - base_depth;
            for (int side = 0; side < 4; side++) {
                float len = std::hypot(corners[side + 1][0] - corners[side][0], corners[side + 1][1] - corners[side][1]);
                uint8_t b = shade[side], d = uint8_t(b * 0.6f);
                int a = skirt.add({ corners[side][0], base_top, corners[side][1], 0, 1, b, b, b });
                int c = skirt.add({ corners[side + 1][0], base_top, corners[side + 1][1], len * 0.35f, 1, b, b, b });
                int e = skirt.add({ corners[side + 1][0], bottom, corners[side + 1][1], len * 0.35f, base_depth * 0.35f, d, d, d });
                int f = skirt.add({ corners[side][0], bottom, corners[side][1], 0, base_depth * 0.35f, d, d, d });
                skirt.quad(a, c, e, f);
            }
            // The underside, so the base looks solid from below too.
            uint8_t u = 60;
            int a = skirt.add({ x0, bottom, z0, 0, 0, u, u, u }), b = skirt.add({ x1, bottom, z0, 0, 0, u, u, u });
            int c = skirt.add({ x1, bottom, z1, 0, 0, u, u, u }), d = skirt.add({ x0, bottom, z1, 0, 0, u, u, u });
            skirt.quad(a, b, c, d);
            textured.push_back(std::move(skirt));
        }

        // The list: the route untextured as the stock dioramas start, the base textured, then the track's objects
        // with their own render state, then what Rush 2 expects back.
        mini.command(0xD7000000, 0xFFFFFFFF);    // G_TEXTURE off
        mini.command(0xE7000000, 0);             // G_RDPPIPESYNC
        mini.command(0xE200001C, 0xC8112078);    // render mode, as the stock dioramas
        mini.command(0xFCFFFFFF, 0xFFFE7C38);    // combiner: shade colour
        emit_group(mini, route);
        emit_group(mini, top);
        mini.command(0xD7000002, 0xFFFFFFFF);    // G_TEXTURE on, tile 0, scale 1
        mini.command(0xE3000F00, 0);             // texture LOD off (the 2049 lists switch it per texture)
        bool combiner = false;
        for (const Group& gq : textured) {
            emit_texture_load(mini, textures[size_t(gq.texture)]);
            if (!combiner) {
                mini.command(0xFC127FFF, 0xFFFFF238); // combiner: texel 0 * shade
                combiner = true;
            }
            emit_group(mini, gq);
        }
        for (const Placed& p : placed) mini.emit(p);
        mini.finish_texels();
        mini.command(0xE7000000, 0);
        mini.command(tlut_rgba16[0], tlut_rgba16[1]);
        if (mini.blend_set) mini.command(blend_rush2[0], blend_rush2[1]);
        if (mini.alpha_compare != 0) mini.command(0xE2001E01, 0);
        mini.command(0xE3000F00, 0);             // texture LOD off, as the stock dioramas end
        mini.command(0xDF000000, 0);             // G_ENDDL
        Bytes& vertices = m.parts[vertices_part];
        if (vertices.size() < 16) return false;
        vertices[vertices.size() - 10] &= 0x7F;  // the last vertex ends the chain
        return true;
    }

    // Track k's miniature at the most detailed level that fits the budget.
    bool build_miniature(const Bytes& rom, int k, TrackModel& m) {
        Bytes geometry, placement, path;
        TopDown print;
        if (!rush2::rom2049::read_file(rom, geometry_file + k, geometry) ||
            !rush2::rom2049::read_file(rom, placement_file + k, placement) ||
            !rush2::rom2049::read_file(rom, path_file + k, path) ||
            !render_top_down(geometry, placement, path, ground_tiles, print)) {
            return false;
        }
        constexpr int levels = int(sizeof(detail_levels) / sizeof(detail_levels[0]));
        for (int d = 0; d < levels; d++) {
            m = TrackModel();
            m.print = print;
            if (!build_track_model_at(geometry, placement, d, m)) return false;
            size_t total = 0;
            for (const Bytes& part : m.parts) total += part.size();
            if (total <= track_budget) break;
        }
        return true;
    }


    // ---------------------------------------------------------------------------------------------------------------
    // Route tube: Rush 2049's own track select model (docs/rush2049_research/menus.md §7)
    //
    // Rush 2049 builds it at runtime in its track select overlay (func_8038CD14, overlay ROM 0xB5C534 at 0x8038A400)
    // from the race track's forward AI path (file 157+k): 100 rings at evenly spaced spine points, each ring a
    // rectangle 200 units wide from 50 units above the road down to a floor 50 units under the lowest spine point, so
    // the "tube" is a flat-topped band with a curtain under it. Its list (texture off, E200001C C8112230, shade
    // combiner) draws each segment as four quads between two rings. Per frame (func_8038A820) it colours the vertices:
    // the bottom ones black, the top ones from the track number (k: bit 0 blue, bit 1 green, bit 2 red, so 1 blue,
    // 2 green, 3 teal, 4 red, 5 purple, 6 yellow), bright (191 / 63) with a white highlight running along the route
    // once every 5 seconds on the selected track, dim (127 / 31) on the map of San Francisco for the others.
    // The R49TRACKn model is that tube with the selected colours frozen with the highlight at the start line, sized
    // like the miniature (longer side 1120 units).

    constexpr bool use_miniature = false;        // build the generated miniature of the track's geometry instead
    constexpr int tube_rings = 100;              // 0x803B7CD8
    constexpr float tube_half_width = 100.0f;    // world units (0x42C8)
    constexpr float tube_lift = 50.0f;           // above the road, and the floor below the lowest point (0x4248)
    constexpr bool tube_highlight = true;        // freeze 2049's moving white highlight at the start line
    constexpr bool tube_close_gaps = false;      // close the tube across any gap, as 2049 does
    constexpr float tube_max_gap = 1000.0f;      // world units: longer closing segments are left out

    bool build_tube(const Bytes& rom, int k, TrackModel& m) {
        Bytes path;
        if (!rush2::rom2049::read_file(rom, path_file + k, path)) return false;
        std::vector<Point> spine;
        std::vector<std::vector<Point>> branches;
        if (!read_path(path, spine, branches) || spine.size() < 4 || path.size() < 12 + 10 * 0x50) return false;
        m.print.spine = spine;
        const int count = int(spine.size());

        // Bounding box and centre of the spine (2049 0x801407B4 max, 0x801407D4 min; centre rounds toward zero).
        int lo[3] = { 32767, 32767, 32767 }, hi[3] = { -32768, -32768, -32768 };
        for (const Point& p : spine) {
            int v[3] = { int(p.x), int(p.y), int(p.z) };
            for (int j = 0; j < 3; j++) { lo[j] = std::min(lo[j], v[j]); hi[j] = std::max(hi[j], v[j]); }
        }
        float centre[3];
        for (int j = 0; j < 3; j++) centre[j] = float(-(hi[j] + lo[j]) / 2);
        const int extent = std::max(hi[0] - lo[0], hi[2] - lo[2]);
        if (extent <= 0) return false;
        const float scale = 2 * diorama_half_size / float(extent);

        // The last ring joins the spine point nearest the finish checkpoint ahead of it (func_800BA61C with the path
        // header's loop checkpoint, s16 at +2; checkpoints at 12 + i * 0x50: f32 position, f32 direction).
        int finish = 0;
        {
            int cp = int16_t(be16(path, 2));
            if (cp >= 0 && cp < 10) {
                size_t r = 12 + size_t(cp) * 0x50;
                float px = as_float(be32(path, r)), pz = as_float(be32(path, r + 8));
                float dx = as_float(be32(path, r + 0xC)), dz = as_float(be32(path, r + 0x14));
                float best = 1e30f;
                for (int i = 0; i < count; i++) {
                    float ex = spine[size_t(i)].x - px, ez = spine[size_t(i)].z - pz;
                    if (ex * dx + ez * dz < 0) continue;
                    float d2 = ex * ex + ez * ez;
                    if (d2 < best) { best = d2; finish = i; }
                }
            }
        }

        // Rings: v0 top +side, v1 top -side, v2 floor -side, v3 floor +side.
        auto at = [&](int i) {
            return Point{ spine[size_t(i)].x + centre[0], spine[size_t(i)].y + centre[1], spine[size_t(i)].z + centre[2] };
        };
        const float floor_y = float(lo[1]) + centre[1] - tube_lift;
        std::vector<std::array<float, 3>> ring(size_t(tube_rings) * 4);
        for (int i = 0; i < tube_rings; i++) {
            Point cur = at(i * count / tube_rings);
            Point next = at(i < tube_rings - 1 ? (i + 1) * count / tube_rings : finish);
            Point prev = i > 0 ? at((i - 1) * count / tube_rings)
                               : Point{ 2 * cur.x - next.x, 2 * cur.y - next.y, 2 * cur.z - next.z };
            float len_cur = std::max(1e-3f, std::hypot(cur.x - next.x, next.z - cur.z));
            float len_prev = std::max(1e-3f, std::hypot(prev.x - cur.x, cur.z - prev.z));
            float px = (tube_half_width * (next.z - cur.z) / len_cur + tube_half_width * (cur.z - prev.z) / len_prev) / 2;
            float pz = (tube_half_width * (cur.x - next.x) / len_cur + tube_half_width * (prev.x - cur.x) / len_prev) / 2;
            float top = cur.y + tube_lift, bottom = floor_y;
            if (i == tube_rings - 1) { top += 0.1f; bottom += 0.1f; }
            ring[size_t(i) * 4 + 0] = { cur.x + px, top, cur.z + pz };
            ring[size_t(i) * 4 + 1] = { cur.x - px, top, cur.z - pz };
            ring[size_t(i) * 4 + 2] = { cur.x - px, bottom, cur.z - pz };
            ring[size_t(i) * 4 + 3] = { cur.x + px, bottom, cur.z + pz };
        }

        // Vertices, chained for the loader's mirror pass like 2049's (flag 0x8000; the last ends the chain).
        Bytes& vertices = m.parts[vertices_part];
        const int bits[3] = { 4, 2, 1 }; // red, green, blue from the track number
        for (int i = 0; i < tube_rings; i++) {
            // Selected-track colour (func_8038A820): a = twice the rings behind the highlight, at most 64.
            int a = tube_highlight ? std::min(2 * ((tube_rings - i) % tube_rings), 64) : 64;
            uint8_t top[3];
            for (int c = 0; c < 3; c++) top[c] = uint8_t((number_of(k) & bits[c]) ? 255 - a : 255 - 3 * a);
            if (i == tube_rings - 1) top[0] = top[1] = top[2] = 255;
            for (int v = 0; v < 4; v++) {
                const auto& p = ring[size_t(i) * 4 + size_t(v)];
                float mv[3];
                for (int j = 0; j < 3; j++) {
                    mv[j] = std::trunc(p[j] * scale);
                    push16(vertices, uint16_t(int16_t(mv[j])));
                }
                push16(vertices, 0x8000);
                push16(vertices, 0);
                push16(vertices, 0);
                bool upper = v < 2;
                vertices.push_back(upper ? top[0] : 0);
                vertices.push_back(upper ? top[1] : 0);
                vertices.push_back(upper ? top[2] : 0);
                vertices.push_back(0xFF);
                m.radius = std::max(m.radius, std::sqrt(mv[0] * mv[0] + mv[1] * mv[1] + mv[2] * mv[2]));
                m.vertices++;
            }
        }
        vertices[vertices.size() - 10] &= 0x7F;

        // The list, as 2049 builds it.
        Bytes& list = m.parts[list_part];
        auto command = [&](uint32_t w0, uint32_t w1, bool vertex_pointer) {
            if (vertex_pointer) m.fixups.push_back({ list_part, uint32_t(list.size() + 4), vertices_part });
            push32(list, w0);
            push32(list, w1);
        };
        command(0xD7000000, 0xFFFFFFFF, false);     // G_TEXTURE off
        command(0xE7000000, 0, false);
        command(0xE200001C, 0xC8112230, false);     // fog, z-buffered opaque surface
        command(0xFCFFFFFF, 0xFFFE7C38, false);     // shade colour
        for (int i = 0; i < tube_rings; i++) {
            if (i < tube_rings - 1) {
                command(0x01008010, uint32_t(i) * 64, true);   // rings i and i + 1 to slots 0-7
            }
            else {
                // 2049 always closes the tube back to ring 0. Track 6's route ends far from its start, so that
                // segment is a sliver across the whole model; it is left out when the gap is that long.
                const auto& a = ring[size_t(i) * 4];
                const auto& b = ring[0];
                if (!tube_close_gaps && std::hypot(a[0] - b[0], a[2] - b[2]) > tube_max_gap) break;
                command(0x01004008, uint32_t(i) * 64, true);   // ring i to slots 0-3
                command(0x01004010, 0, true);                  // ring 0 to slots 4-7
            }
            command(0x06000802, 0x000A0208, false);  // top
            command(0x06020A04, 0x000C040A, false);  // -side
            command(0x06040C06, 0x000E060C, false);  // floor
            command(0x06060E00, 0x0008000E, false);  // +side
            m.triangles += 8;
        }
        command(0xD7000002, 0xFFFFFFFF, false);     // G_TEXTURE on, as 2049's list ends
        command(0xDF000000, 0, false);
        m.objects = m.objects_drawn = 1;
        return true;
    }

    bool build_track_model(const Bytes& rom, int k, TrackModel& m) {
        return use_miniature && k <= rush2::track2049::track_count ? build_miniature(rom, k, m) : build_tube(rom, k, m);
    }

    // ---------------------------------------------------------------------------------------------------------------
    // Logo

    constexpr int logo_w = 128, logo_h = 32;

    // 5x7 capitals and digits, one byte per row, bit 4 = leftmost column.
    const std::map<char, std::array<uint8_t, 7>> font = {
        { 'A', { 0x0E, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11 } }, { 'B', { 0x1E, 0x11, 0x11, 0x1E, 0x11, 0x11, 0x1E } },
        { 'C', { 0x0E, 0x11, 0x10, 0x10, 0x10, 0x11, 0x0E } }, { 'E', { 0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x1F } },
        { 'H', { 0x11, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11 } }, { 'K', { 0x11, 0x12, 0x14, 0x18, 0x14, 0x12, 0x11 } },
        { 'L', { 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x1F } }, { 'N', { 0x11, 0x19, 0x15, 0x13, 0x11, 0x11, 0x11 } },
        { 'O', { 0x0E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E } },
        { 'R', { 0x1E, 0x11, 0x11, 0x1E, 0x14, 0x12, 0x11 } }, { 'S', { 0x0F, 0x10, 0x10, 0x0E, 0x01, 0x01, 0x1E } },
        { 'T', { 0x1F, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04 } }, { 'U', { 0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E } },
        { '0', { 0x0E, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0E } }, { '1', { 0x04, 0x0C, 0x04, 0x04, 0x04, 0x04, 0x0E } },
        { '2', { 0x0E, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1F } }, { '3', { 0x1F, 0x02, 0x04, 0x02, 0x01, 0x11, 0x0E } },
        { '4', { 0x02, 0x06, 0x0A, 0x12, 0x1F, 0x02, 0x02 } }, { '5', { 0x1F, 0x10, 0x1E, 0x01, 0x01, 0x11, 0x0E } },
        { '6', { 0x06, 0x08, 0x10, 0x1E, 0x11, 0x11, 0x0E } }, { '9', { 0x0E, 0x11, 0x11, 0x0F, 0x01, 0x02, 0x0C } },
    };

    // Palette: 0 transparent, 1-3 text, 32-255 a 7x8x4 colour cube for the icon.
    constexpr uint8_t ink_big = 1, ink_small = 2, ink_outline = 3, cube_base = 32;

    // advance: pixels from one character to the next, 0 for 6 x scale.
    void draw_text(std::array<uint8_t, logo_w * logo_h>& img, const std::string& text, int x0, int y0, int scale, uint8_t ink,
                   int advance = 0) {
        int x = x0;
        for (char ch : text) {
            auto g = font.find(ch);
            if (g != font.end()) {
                for (int row = 0; row < 7; row++) {
                    for (int col = 0; col < 5; col++) {
                        if (!(g->second[row] & (0x10 >> col))) continue;
                        for (int sy = 0; sy < scale; sy++) {
                            for (int sx = 0; sx < scale; sx++) {
                                int px = x + col * scale + sx, py = y0 + row * scale + sy;
                                if (px >= 0 && px < logo_w && py >= 0 && py < logo_h) {
                                    img[py * logo_w + px] = ink;
                                }
                            }
                        }
                    }
                }
            }
            x += advance > 0 ? advance : 6 * scale;
        }
    }

    // TPICk (stunt arena n: SPICn, the obstacle course: OPIC1) as RGBA, upright (2049 stores its thumbnails bottom-up). Returns false if not found.
    bool read_thumbnail(const std::vector<uint8_t>& ui, int k, std::vector<std::array<uint8_t, 4>>& rgba, int& w, int& h) {
        if (ui.size() < 4) return false;
        // Chunk directory: IMAG, TXHD, PLHD (docs/rush2049_research/geometry.md §3).
        uint32_t dir = be32(ui, 0);
        uint32_t imag = 0, txhd = 0, txhd_n = 0, plhd = 0, plhd_n = 0;
        for (uint32_t o = dir; o + 12 <= ui.size(); o += 12) {
            std::string tag(reinterpret_cast<const char*>(&ui[o]), 4);
            if (!std::all_of(tag.begin(), tag.end(), ::isalpha)) break;
            if (tag == "IMAG") imag = be32(ui, o + 4);
            if (tag == "TXHD") { txhd = be32(ui, o + 4); txhd_n = be32(ui, o + 8); }
            if (tag == "PLHD") { plhd = be32(ui, o + 4); plhd_n = be32(ui, o + 8); }
        }
        std::string name = (is_obstacle(k) ? "OPIC" : is_stunt(k) ? "SPIC" : "TPIC") + std::to_string(number_of(k));
        auto name_at = [&](uint32_t o) { return std::string(reinterpret_cast<const char*>(&ui[o]), strnlen(reinterpret_cast<const char*>(&ui[o]), 16)); };
        for (uint32_t i = 0; i < txhd_n; i++) {
            uint32_t r = txhd + i * 0x24;
            if (name_at(r) != name) continue;
            w = be16(ui, r + 16); h = be16(ui, r + 18);
            uint32_t texels = imag + be32(ui, r + 24);
            for (uint32_t j = 0; j < plhd_n; j++) {
                uint32_t p = plhd + j * 0x18;
                if (name_at(p) != name) continue;
                uint32_t pal = imag + be32(ui, p + 20);
                if (texels + uint32_t(w * h) > ui.size() || pal + 512 > ui.size()) return false;
                rgba.resize(size_t(w * h));
                for (int y = 0; y < h; y++) {
                    for (int x = 0; x < w; x++) {
                        uint16_t c = be16(ui, pal + ui[texels + (h - 1 - y) * w + x] * 2);
                        rgba[y * w + x] = { uint8_t((c >> 11) << 3), uint8_t(((c >> 6) & 31) << 3), uint8_t(((c >> 1) & 31) << 3), uint8_t((c & 1) ? 255 : 0) };
                    }
                }
                return true;
            }
        }
        return false;
    }

    // 128x32 CI8 texels (rows bottom-up, as Rush 2 stores its logos) and a 256-entry RGBA5551 palette.
    void build_logo(const std::vector<uint8_t>& ui, int k, std::vector<uint8_t>& texels, std::vector<uint8_t>& palette) {
        std::array<uint16_t, 256> pal{};
        pal[ink_big] = rgba5551(255, 196, 24, 1);
        pal[ink_small] = rgba5551(235, 235, 245, 1);
        pal[ink_outline] = rgba5551(24, 10, 0, 1);
        for (int i = 0; i < 224; i++) {
            int r = i % 7, g = (i / 7) % 8, b = i / 56;
            pal[cube_base + i] = rgba5551(r * 255 / 6, g * 255 / 7, b * 255 / 3, 1);
        }

        std::array<uint8_t, logo_w * logo_h> img{};
        // Icon: the 2049 thumbnail shrunk to 32x32, alpha by majority.
        std::vector<std::array<uint8_t, 4>> thumb;
        int tw = 0, th = 0;
        if (read_thumbnail(ui, k, thumb, tw, th) && tw >= 32 && th >= 32) {
            int fx = tw / 32, fy = th / 32;
            for (int y = 0; y < 32; y++) {
                for (int x = 0; x < 32; x++) {
                    int sum[3] = {}, opaque = 0;
                    for (int sy = 0; sy < fy; sy++) {
                        for (int sx = 0; sx < fx; sx++) {
                            const auto& c = thumb[(y * fy + sy) * tw + x * fx + sx];
                            if (c[3]) { sum[0] += c[0]; sum[1] += c[1]; sum[2] += c[2]; opaque++; }
                        }
                    }
                    if (opaque * 2 < fx * fy) continue;
                    int r = std::lround(sum[0] / float(opaque) * 6 / 255), g = std::lround(sum[1] / float(opaque) * 7 / 255),
                        b = std::lround(sum[2] / float(opaque) * 3 / 255);
                    img[y * logo_w + x] = uint8_t(cube_base + r + g * 7 + b * 56);
                }
            }
        }
        // Name, with a one-pixel outline around the text.
        std::array<uint8_t, logo_w * logo_h> text{};
        draw_text(text, "RUSH 2049", 38, 2, 1, ink_small);
        if (is_obstacle(k)) {
            draw_text(text, "OBSTACLE", 36, 13, 2, ink_big, 11);  // one pixel apart, to fit
        }
        else {
            draw_text(text, (is_stunt(k) ? "STUNT " : "TRACK ") + std::to_string(number_of(k)), 36, 13, 2, ink_big);
        }
        for (int y = 0; y < logo_h; y++) {
            for (int x = 34; x < logo_w; x++) {
                if (text[y * logo_w + x]) {
                    img[y * logo_w + x] = text[y * logo_w + x];
                    continue;
                }
                bool near_ink = false;
                for (int dy = -1; dy <= 1; dy++) for (int dx = -1; dx <= 1; dx++) {
                    int px = x + dx, py = y + dy;
                    if (px >= 0 && px < logo_w && py >= 0 && py < logo_h && text[py * logo_w + px]) near_ink = true;
                }
                if (near_ink) img[y * logo_w + x] = ink_outline;
            }
        }

        texels.resize(logo_w * logo_h);
        for (int y = 0; y < logo_h; y++) {
            memcpy(&texels[(logo_h - 1 - y) * logo_w], &img[y * logo_w], logo_w);
        }
        palette.clear();
        for (uint16_t c : pal) push16(palette, c);
    }

    // ---------------------------------------------------------------------------------------------------------------
    // Container

    std::string name_of(const std::vector<uint8_t>& d, size_t o) {
        return std::string(reinterpret_cast<const char*>(&d[o]), strnlen(reinterpret_cast<const char*>(&d[o]), 15));
    }
    std::array<uint8_t, 16> name_bytes(const std::string& name) {
        std::array<uint8_t, 16> b{};
        memcpy(b.data(), name.data(), std::min<size_t>(name.size(), 15));
        return b;
    }

    // Moves a stock model list's pointers into [from, ...) by delta, as the relocator (func_8007786C) finds them.
    void shift_list(Bytes& d, uint32_t list, uint32_t from, uint32_t delta) {
        for (uint32_t pc = list; pc + 8 <= d.size(); pc += 8) {
            uint8_t op = d[pc];
            if (op == 0xDF) return;
            bool pointer = op == 0x01 || op == 0xDD || op == 0xDE || op == 0xDA || op == 0xDC || op == 0xFD ||
                           op == 0xFE || op == 0xFF || (op == 0xE0 && d[pc + 1] == 1) ||
                           (op == 0xE1 && pc + 16 <= d.size() && (d[pc + 8] == 0x04 || d[pc + 8] == 0xDD));
            uint32_t w1 = be32(d, pc + 4);
            if (pointer && (w1 & 0xFFFFFF) >= from) put32(d, pc + 4, (w1 & 0xFF000000) | ((w1 & 0xFFFFFF) + delta));
        }
    }
}

std::string rush2::track2049::menu_model_name(int k) {
    if (is_obstacle(k)) return "R49OBSTACLE";
    return (is_stunt(k) ? "R49STUNT" : "R49TRACK") + std::to_string(number_of(k));
}

std::string rush2::track2049::menu_logo_name(int k) {
    if (is_obstacle(k)) return "R49OLOGO";
    return (is_stunt(k) ? "R49SLOGO" : "R49LOGO") + std::to_string(number_of(k));
}

bool rush2::track2049::build_menu_container(const std::vector<uint8_t>& asset3, const std::vector<uint8_t>& rom2049,
                                            std::vector<uint8_t>& out) {
    if (asset3.size() < 0x28) {
        return false;
    }
    uint32_t h[10];
    for (int i = 0; i < 10; i++) h[i] = be32(asset3, i * 4);
    if (h[0] + h[4] * 0x34 > asset3.size() || h[1] + h[4] * 0x18 > asset3.size() ||
        h[2] + h[5] * 0x20 > asset3.size() || h[3] + h[6] * 0x18 > asset3.size() || h[7] > h[8] || h[8] > asset3.size() ||
        h[8] % 8) {
        return false;
    }
    std::vector<uint8_t> ui;
    if (!rush2::rom2049::read_file(rom2049, ui_file, ui)) {
        return false;
    }
    std::vector<int> ks;
    for (int k = 1; k <= track_count; k++) ks.push_back(k);
    for (int n = 0; n < stunt_count; n++) ks.push_back(stunt_first + n);
    ks.push_back(obstacle);
    std::vector<TrackModel> tracks(ks.size());
    for (size_t i = 0; i < ks.size(); i++) {
        if (!build_track_model(rom2049, ks[i], tracks[i])) {
            return false;
        }
    }

    // Layout: Rush 2's data up to the end of its texture-load lists ([8]), the 2049 texture-load lists right after
    // (so the loader's linear SETTIMG scan of [7]..[8] covers them), the rest of Rush 2's data moved up by their size,
    // then the dioramas' texels, vertices and lists, the logos and the tables.
    const uint32_t insert_at = h[8];
    std::vector<std::array<uint32_t, part_count>> base(tracks.size());
    Bytes loads;
    for (size_t t = 0; t < tracks.size(); t++) {
        base[t][loads_part] = insert_at + uint32_t(loads.size());
        loads.insert(loads.end(), tracks[t].parts[loads_part].begin(), tracks[t].parts[loads_part].end());
        align(loads, 8);
    }
    const uint32_t delta = uint32_t(loads.size());
    out.assign(asset3.begin(), asset3.begin() + insert_at);
    out.insert(out.end(), loads.begin(), loads.end());
    out.insert(out.end(), asset3.begin() + insert_at, asset3.end());
    auto moved = [&](uint32_t offset) { return offset >= insert_at ? offset + delta : offset; };

    struct Model { std::string name; std::vector<uint8_t> rec, name_rec; };
    struct Texture { std::string name; std::vector<uint8_t> rec; std::string palette; };
    struct Palette { std::string name; std::vector<uint8_t> rec; };
    std::vector<Model> models;
    std::vector<Texture> textures;
    std::vector<Palette> palettes;
    std::set<uint32_t> shifted;
    for (uint32_t i = 0; i < h[4]; i++) {
        Model mo{ name_of(asset3, h[1] + i * 0x18),
            std::vector<uint8_t>(asset3.begin() + h[0] + i * 0x34, asset3.begin() + h[0] + (i + 1) * 0x34),
            std::vector<uint8_t>(asset3.begin() + h[1] + i * 0x18, asset3.begin() + h[1] + (i + 1) * 0x18) };
        for (uint32_t l = 0; l < 4; l++) {
            uint32_t list = be32(mo.rec, 4 + l * 12 + 8);
            if (list == 0) continue;
            put32(mo.rec, 4 + l * 12 + 8, moved(list));
            if (shifted.insert(moved(list)).second) shift_list(out, moved(list), insert_at, delta);
        }
        models.push_back(std::move(mo));
    }
    for (uint32_t i = 0; i < h[6]; i++) {
        Palette p{ name_of(asset3, h[3] + i * 0x18),
            std::vector<uint8_t>(asset3.begin() + h[3] + i * 0x18, asset3.begin() + h[3] + (i + 1) * 0x18) };
        put32(p.rec, 20, moved(be32(p.rec, 20)));
        palettes.push_back(std::move(p));
    }
    for (uint32_t i = 0; i < h[5]; i++) {
        uint32_t r = h[2] + i * 0x20;
        int16_t pal = int16_t(be16(asset3, r + 0x16));
        Texture t{ name_of(asset3, r), std::vector<uint8_t>(asset3.begin() + r, asset3.begin() + r + 0x20),
            pal >= 0 && uint32_t(pal) < h[6] ? palettes[size_t(pal)].name : std::string() };
        put32(t.rec, 24, moved(be32(t.rec, 24)));
        textures.push_back(std::move(t));
    }

    for (size_t i = 0; i < ks.size(); i++) {
        int k = ks[i];
        TrackModel& tm = tracks[i];
        auto& b = base[i];
        for (int part : { texels_part, vertices_part, list_part }) {
            align(out, 8);
            b[size_t(part)] = uint32_t(out.size());
            out.insert(out.end(), tm.parts[part].begin(), tm.parts[part].end());
        }
        for (const Fixup& f : tm.fixups) {
            uint32_t at = b[size_t(f.in)] + f.at;
            uint32_t w = be32(out, at);
            put32(out, at, (w & 0xFF000000) | ((w & 0xFFFFFF) + b[size_t(f.to)]));
        }
        std::string model_name = menu_model_name(k);
        std::vector<uint8_t> rec(0x34, 0), name_rec(0x18, 0);
        put32(rec, 0, 1);
        put32(rec, 4, 0);                     // LOD 0: texture handle 0, flags 0, distance 0 (always drawn)
        put32(rec, 8, 0);
        put32(rec, 12, b[list_part]);
        auto nb = name_bytes(model_name);
        memcpy(name_rec.data(), nb.data(), 16);
        put32(name_rec, 16, float_bits(tm.radius / 16.0f));
        models.push_back({ model_name, rec, name_rec });

        std::vector<uint8_t> texels, palette;
        build_logo(ui, k, texels, palette);
        align(out, 8);
        uint32_t texel_off = uint32_t(out.size());
        out.insert(out.end(), texels.begin(), texels.end());
        uint32_t palette_off = uint32_t(out.size());
        out.insert(out.end(), palette.begin(), palette.end());
        std::string logo_name = menu_logo_name(k);
        std::vector<uint8_t> trec(0x20, 0), prec(0x18, 0);
        auto lb = name_bytes(logo_name);
        memcpy(trec.data(), lb.data(), 16);
        trec[16] = 0; trec[17] = 128; trec[18] = 0; trec[19] = 32;   // w, h
        trec[20] = 0x01; trec[21] = 0x02;                              // as Rush 2's logos
        put32(trec, 24, texel_off);
        put32(trec, 28, 0x48008000);
        memcpy(prec.data(), lb.data(), 16);
        put32(prec, 16, 0x00FF8000);
        put32(prec, 20, palette_off);
        textures.push_back({ logo_name, trec, logo_name });
        palettes.push_back({ logo_name, prec });
    }

    auto by_name = [](const auto& a, const auto& b) { return a.name < b.name; };
    std::sort(models.begin(), models.end(), by_name);
    std::sort(textures.begin(), textures.end(), by_name);
    std::sort(palettes.begin(), palettes.end(), by_name);

    align(out, 8);
    uint32_t model_off = uint32_t(out.size());
    for (const Model& m : models) out.insert(out.end(), m.rec.begin(), m.rec.end());
    uint32_t texture_off = uint32_t(out.size());
    for (Texture& t : textures) {
        int16_t index = -1;
        for (size_t i = 0; i < palettes.size(); i++) {
            if (palettes[i].name == t.palette) index = int16_t(i);
        }
        t.rec[0x16] = uint8_t(uint16_t(index) >> 8);
        t.rec[0x17] = uint8_t(index);
        out.insert(out.end(), t.rec.begin(), t.rec.end());
    }
    uint32_t palette_off = uint32_t(out.size());
    for (const Palette& p : palettes) out.insert(out.end(), p.rec.begin(), p.rec.end());
    uint32_t name_off = uint32_t(out.size());
    for (const Model& m : models) out.insert(out.end(), m.name_rec.begin(), m.name_rec.end());
    put32(out, 0, model_off);
    put32(out, 4, name_off);
    put32(out, 8, texture_off);
    put32(out, 12, palette_off);
    put32(out, 16, uint32_t(models.size()));
    put32(out, 20, uint32_t(textures.size()));
    put32(out, 24, uint32_t(palettes.size()));
    put32(out, 32, insert_at + delta);        // [8]: the texture-load lists now end after the 2049 ones
    return true;
}

bool rush2::track2049::build_race_logo(const std::vector<uint8_t>& logo, const std::vector<uint8_t>& rom2049, int k,
                                       std::vector<uint8_t>& out) {
    // Rush 2's logo containers hold one texture and its palette; overwrite their data in place.
    if (logo.size() < 0x28 || be32(logo, 20) != 1 || be32(logo, 24) != 1) {
        return false;
    }
    uint32_t tex = be32(logo, 8), pal = be32(logo, 12);
    uint32_t texels = be32(logo, tex + 24), palette = be32(logo, pal + 20);
    if (be16(logo, tex + 16) != logo_w || be16(logo, tex + 18) != logo_h || texels + logo_w * logo_h > logo.size() ||
        palette + 512 > logo.size()) {
        return false;
    }
    std::vector<uint8_t> ui;
    if (!rush2::rom2049::read_file(rom2049, ui_file, ui)) {
        return false;
    }
    std::vector<uint8_t> t, p;
    build_logo(ui, k, t, p);
    out = logo;
    memcpy(&out[texels], t.data(), t.size());
    memcpy(&out[palette], p.data(), p.size());
    return true;
}
