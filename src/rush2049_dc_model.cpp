// N64 Rush 2049 model containers made from the Dreamcast disc's. See docs/rush2049_research/dreamcast.md ("Models")
// for the disc's format and geometry.md section 3 for the N64 container this writes.
//
// A disc container (little endian) holds IMAG texels in PowerVR formats, DHXT 0x30-byte texture records, DHBO
// 0x48-byte object records and the objects' command streams (triangle strips with cached positions, uvs and colors).
// The N64 one (big endian) holds IMAG, TXLD texture-load lists, OBHD 0x58-byte object records, PLHD palettes, TXHD
// 0x24-byte texture records and OBJS F3DEX2 lists and vertices. Each disc texture becomes one N64 texture scaled down to
// fit TMEM (RGBA16, or RGBA32 for ARGB4444 ones, which carry graded alpha), loaded by its own TXLD list; each object
// LOD becomes one list drawing its strips as triangles in batches of up to 32 vertices, with the render mode the
// texture's pixel format asks for (the disc's loader 0x8C026104 picks the PowerVR list the same way: ARGB1555
// punch-through, ARGB4444 translucent, RGB565 opaque). Positions are the disc's times 16 (the N64's fixed point);
// texture coordinates are the N64's 10.5 texels less half a texel.
//
// Names: track geometry keeps the disc's names (its placement file comes from the disc too). Other files are made in
// the N64's object order and names from the table tools/rush2049/dc_names.py generates (rush2049_dc_names.inc), which
// also notes where the N64 draws an object differently (a texture taken by code on one version but not the other).

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <unordered_map>

#include "rush2049_dc_internal.h"

namespace {
    using Bytes = std::vector<uint8_t>;

    // Thrown on data that doesn't convert; the line says where.
    struct Bad {
        int line;
    };

    // ------------------------------------------------------------------------------------------------------------
    // Name tables

    struct Rename {
        const char* dc;
        const char* n64;
    };
    const Rename renames[] = {
#define RENAME(dc, n64) { dc, n64 },
#include "rush2049_dc_names.inc"
    };

    struct FileObject {
        int file;
        const char* n64;
        const char* dc;
        int flags;          // N64 LOD flags, or -1 to keep the disc's
        const char* handle; // texture the texture-swap handle names ("" = the disc's handle)
        const char* bind;   // texture the list loads where the disc takes one by code ("" = none)
        uint32_t mode;      // render mode for a texture taken by code (0 = by the texture's format)
    };
    const FileObject file_objects[] = {
#define OBJECT(file, n64, dc, flags, handle, bind, mode) { file, n64, dc, flags, handle, bind, mode },
#include "rush2049_dc_names.inc"
    };

    struct Tinted {
        int file;
        const char* n64;
        const char* dc;
        uint32_t rgb;
    };
    const Tinted tinted[] = {
#define TINTED(file, n64, dc, rgb) { file, n64, dc, rgb },
#include "rush2049_dc_names.inc"
    };

    // Textures a non-track N64 file names (code looks them up by name), with the N64's size, which they don't exceed.
    struct NamedTexture {
        int file;
        const char* name;
        int w, h;
    };
    const NamedTexture named_textures[] = {
#define TEXTURE(file, name, w, h) { file, name, w, h },
#include "rush2049_dc_names.inc"
    };

    // Sizes of the non-track N64 files: the made ones stay within them (code loads some into fixed windows).
    struct FileSize {
        int file;
        uint32_t bytes;
    };
    const FileSize file_sizes[] = {
#define SIZE(file, bytes) { file, bytes },
#include "rush2049_dc_names.inc"
    };

    // The render mode the N64 draws a texture with, per N64 file.
    struct TexMode {
        int file;
        const char* name;
        uint32_t mode;
    };
    const TexMode texture_modes[] = {
#define TEXMODE(file, name, mode) { file, name, mode },
#include "rush2049_dc_names.inc"
    };

    // ------------------------------------------------------------------------------------------------------------
    // Byte access

    uint32_t le32(const Bytes& d, size_t o) {
        if (o + 4 > d.size()) throw Bad{ __LINE__ };
        return d[o] | (d[o + 1] << 8) | (d[o + 2] << 16) | ((uint32_t)d[o + 3] << 24);
    }
    uint16_t le16(const Bytes& d, size_t o) {
        if (o + 2 > d.size()) throw Bad{ __LINE__ };
        return uint16_t(d[o] | (d[o + 1] << 8));
    }
    float lef(const Bytes& d, size_t o) {
        uint32_t v = le32(d, o);
        float f;
        memcpy(&f, &v, 4);
        return f;
    }
    void put32(Bytes& d, size_t o, uint32_t v) {
        d[o] = uint8_t(v >> 24);
        d[o + 1] = uint8_t(v >> 16);
        d[o + 2] = uint8_t(v >> 8);
        d[o + 3] = uint8_t(v);
    }
    void put16(Bytes& d, size_t o, uint16_t v) {
        d[o] = uint8_t(v >> 8);
        d[o + 1] = uint8_t(v);
    }
    void add32(Bytes& d, uint32_t v) {
        d.resize(d.size() + 4);
        put32(d, d.size() - 4, v);
    }
    void align8(Bytes& d) {
        d.resize((d.size() + 7) & ~size_t(7));
    }
    std::string cstr(const Bytes& d, size_t o, size_t n) {
        if (o + n > d.size()) throw Bad{ __LINE__ };
        return std::string((const char*)&d[o], strnlen((const char*)&d[o], n));
    }

    // ------------------------------------------------------------------------------------------------------------
    // The disc's container

    struct DcLod {
        uint16_t handle = 0, flags = 0;
        uint32_t distance = 0;  // float bits
        uint32_t stream = 0;
    };
    struct DcObject {
        std::string name;
        uint32_t radius = 0;    // float bits
        uint16_t kind = 0;
        int16_t lod_count = 0;
        DcLod lods[4];
    };
    struct DcTexture {
        std::string name;
        int w = 0, h = 0;
        int pixel = 0;          // 0 ARGB4444, 1 RGB565, 2 ARGB1555
        uint32_t data = 0;      // from IMAG
        uint32_t flags = 0;     // 0x04000000 mipmapped
        uint32_t layout = 0;    // 0x2000 VQ, 0x4000 twiddled, neither linear
    };
    struct DcModel {
        const Bytes* d = nullptr;
        std::map<std::string, std::pair<uint32_t, uint32_t>> chunks;
        uint32_t imag = 0;
        std::vector<DcObject> objects;
        std::vector<DcTexture> textures;
    };

    DcModel parse(const Bytes& d) {
        DcModel m;
        m.d = &d;
        uint32_t n = le32(d, 4);
        if (n > 64 || (uint64_t)n * 12 > d.size()) throw Bad{ __LINE__ };
        size_t dir = d.size() - n * 12;
        for (uint32_t i = 0; i < n; i++) {
            size_t e = dir + i * 12;
            std::string tag{ (char)d[e + 3], (char)d[e + 2], (char)d[e + 1], (char)d[e] };
            m.chunks[tag] = { le32(d, e + 4), le32(d, e + 8) };
        }
        if (!m.chunks.contains("IMAG") || !m.chunks.contains("OBHD") || !m.chunks.contains("TXHD")) throw Bad{ __LINE__ };
        m.imag = m.chunks["IMAG"].first;
        auto [obhd, objects] = m.chunks["OBHD"];
        for (uint32_t i = 0; i < objects; i++) {
            size_t r = obhd + i * 0x48;
            DcObject o;
            o.name = cstr(d, r, 16);
            o.radius = le32(d, r + 16);
            o.kind = le16(d, r + 20);
            o.lod_count = (int16_t)le16(d, r + 22);
            if (o.lod_count < 0 || o.lod_count > 4) throw Bad{ __LINE__ };
            for (int j = 0; j < 4; j++) {
                size_t l = r + 0x18 + j * 12;
                o.lods[j] = { le16(d, l), le16(d, l + 2), le32(d, l + 4), le32(d, l + 8) };
            }
            m.objects.push_back(o);
        }
        auto [txhd, textures] = m.chunks["TXHD"];
        for (uint32_t i = 0; i < textures; i++) {
            size_t r = txhd + i * 0x30;
            DcTexture t;
            t.name = cstr(d, r, 16);
            t.w = le16(d, r + 16);
            t.h = le16(d, r + 18);
            t.pixel = d[r + 21];
            t.data = le32(d, r + 24);
            t.flags = le32(d, r + 28);
            t.layout = le32(d, r + 32);
            if (t.w < 1 || t.h < 1 || t.w > 1024 || t.h > 1024 || (t.w & (t.w - 1)) || (t.h & (t.h - 1)) || t.pixel > 2) {
                throw Bad{ __LINE__ };
            }
            m.textures.push_back(t);
        }
        return m;
    }

    // ------------------------------------------------------------------------------------------------------------
    // PowerVR textures

    struct Rgba {
        uint8_t r, g, b, a;
    };

    Rgba texel(uint16_t v, int pixel) {
        switch (pixel) {
        case 0:
            return { uint8_t(((v >> 8) & 15) * 17), uint8_t(((v >> 4) & 15) * 17), uint8_t((v & 15) * 17), uint8_t((v >> 12) * 17) };
        case 1:
            return { uint8_t(((v >> 11) & 31) * 255 / 31), uint8_t(((v >> 5) & 63) * 255 / 63), uint8_t((v & 31) * 255 / 31), 255 };
        default:
            return { uint8_t(((v >> 10) & 31) * 255 / 31), uint8_t(((v >> 5) & 31) * 255 / 31), uint8_t((v & 31) * 255 / 31),
                     uint8_t(v & 0x8000 ? 255 : 0) };
        }
    }

    // Index of (x, y) in a twiddled square: the bits of y and x interleaved, y's in the even places.
    uint32_t twiddle(uint32_t x, uint32_t y) {
        uint32_t o = 0;
        for (int b = 0; (1u << b) <= std::max(x, y); b++) {
            o |= ((y >> b) & 1) << (2 * b) | ((x >> b) & 1) << (2 * b + 1);
        }
        return o;
    }

    int log2i(int v) {
        int l = 0;
        while ((1 << l) < v) l++;
        return l;
    }

    // Offset of a mipmapped texture's largest level: VQ in index bytes past the 2 KB codebook, other layouts in texels
    // (the smaller levels come first, the 1x1 one after a 3-texel pad).
    constexpr uint32_t vq_mip[11] = { 0x0, 0x1, 0x2, 0x6, 0x16, 0x56, 0x156, 0x556, 0x1556, 0x5556, 0x15556 };
    constexpr uint32_t mip16[11] = { 0x3, 0x4, 0x8, 0x18, 0x58, 0x158, 0x558, 0x1558, 0x5558, 0x15558, 0x55558 };

    // Largest level of a disc texture. Rectangles are twiddled as squares of the shorter side, one after another.
    std::vector<Rgba> decode(const DcModel& m, const DcTexture& t) {
        const Bytes& d = *m.d;
        int w = t.w, h = t.h, s = std::min(w, h);
        bool mip = (t.flags & 0x04000000) != 0;
        size_t at = (size_t)m.imag + t.data;
        std::vector<Rgba> out((size_t)w * h);
        if (t.layout & 0x2000) {
            size_t idx = at + 2048 + (mip ? vq_mip[log2i(w)] : 0);
            int hs = std::max(1, s / 2);
            if (idx + (size_t)w * h / 4 > d.size()) throw Bad{ __LINE__ };
            for (int y = 0; y < h; y += 2) {
                for (int x = 0; x < w; x += 2) {
                    int bx = x / 2, by = y / 2;
                    size_t blk = (size_t)(bx / hs + by / hs) * hs * hs;
                    size_t code = at + (size_t)d[idx + blk + twiddle(bx % hs, by % hs)] * 8;
                    for (int k = 0; k < 4; k++) {
                        int px = x + (k >> 1), py = y + (k & 1);
                        if (px < w && py < h) out[(size_t)py * w + px] = texel(le16(d, code + k * 2), t.pixel);
                    }
                }
            }
        }
        else if (t.layout & 0x4000) {
            size_t base = at + (mip ? (size_t)mip16[log2i(w)] * 2 : 0);
            if (base + (size_t)w * h * 2 > d.size()) throw Bad{ __LINE__ };
            for (int y = 0; y < h; y++) {
                for (int x = 0; x < w; x++) {
                    size_t blk = (size_t)(x / s + y / s) * s * s;
                    out[(size_t)y * w + x] = texel(le16(d, base + 2 * (blk + twiddle(x % s, y % s))), t.pixel);
                }
            }
        }
        else {
            if (at + (size_t)w * h * 2 > d.size()) throw Bad{ __LINE__ };
            for (size_t i = 0; i < out.size(); i++) out[i] = texel(le16(d, at + i * 2), t.pixel);
        }
        return out;
    }

    // ------------------------------------------------------------------------------------------------------------
    // N64 textures

    // The N64's render modes for its models: opaque, alpha-tested edges, translucent, translucent writing depth.
    constexpr uint32_t rm_opaque = 0xC8112230, rm_edge = 0xC8113278, rm_translucent = 0xC8104A50;

    // A texture's render mode: the one the N64 draws the same texture with in the same file (the N64 names few of a
    // track's textures), else by its alpha the way the N64's track textures are drawn: mostly see-through (glass,
    // water, smoke) translucent, any clear texels (tree cards, fences: the disc's soft-edged ARGB4444 cutouts)
    // alpha-tested edges, which write depth only where they're solid, else opaque. Drawing cutouts translucent let
    // farther objects drawn later show through them (the Golden Gate through the trees); translucent writing depth
    // blocked what's behind their clear parts.
    uint32_t texture_mode(int file, const std::string& name, const std::vector<Rgba>& px) {
        for (const TexMode& t : texture_modes) {
            if (t.file == file && name == t.name) return t.mode;
        }
        size_t clear = 0, graded = 0;
        for (const Rgba& p : px) {
            if (p.a < 255) clear++;
            if (p.a >= 16 && p.a < 240) graded++;
        }
        return graded * 2 > px.size() ? rm_translucent : clear ? rm_edge : rm_opaque;
    }
    // Combiners (2-cycle, the second passing the first on): texel times shade with the texel's alpha, as the N64's
    // own unmipmapped lists; shade with alpha 1 for untextured strips.
    constexpr uint32_t cc_textured[2] = { 0xFC127FFF, 0xFFFFF238 };
    constexpr uint32_t cc_shade[2] = { 0xFCFFFFFF, 0xFFFE7C38 };

    struct OutTexture {
        std::string name;
        int w = 0, h = 0;           // N64 size
        bool rgba32 = false;
        uint32_t mode = rm_opaque;
        uint32_t texels = 0;        // offset in IMAG
        uint32_t list = 0;          // offset of the load list in TXLD
        Bytes data;
        bool shrunk = false;        // smaller than the disc's
        int source = -1;            // the disc's texture record
        uint32_t tint = 0xFFFFFF;
        bool ci8 = false;           // CI8 texels of the file's palette (a car's, paint_car)
        std::vector<Rgba> small;    // the texels at the N64 size, before packing
    };

    // Box-filtered to w x h (alpha-weighted color, so clear texels don't darken their neighbors).
    std::vector<Rgba> shrink(const std::vector<Rgba>& in, int iw, int ih, int w, int h) {
        std::vector<Rgba> out((size_t)w * h);
        int fx = iw / w, fy = ih / h;
        for (int y = 0; y < h; y++) {
            for (int x = 0; x < w; x++) {
                uint32_t r = 0, g = 0, b = 0, a = 0, rr = 0, gg = 0, bb = 0;
                for (int j = 0; j < fy; j++) {
                    for (int i = 0; i < fx; i++) {
                        const Rgba& p = in[(size_t)(y * fy + j) * iw + x * fx + i];
                        r += p.r * p.a;
                        g += p.g * p.a;
                        b += p.b * p.a;
                        a += p.a;
                        rr += p.r;
                        gg += p.g;
                        bb += p.b;
                    }
                }
                uint32_t n = fx * fy;
                if (a > 0) out[(size_t)y * w + x] = { uint8_t(r / a), uint8_t(g / a), uint8_t(b / a), uint8_t(a / n) };
                else out[(size_t)y * w + x] = { uint8_t(rr / n), uint8_t(gg / n), uint8_t(bb / n), 0 };
            }
        }
        return out;
    }

    // level: TMEM's texel limit is halved that many times (to fit a file's size budget).
    OutTexture make_texture(const std::string& name, const std::vector<Rgba>& px, int w, int h, int pixel, uint32_t tint,
                            int level, int max_w, int max_h, uint32_t mode) {
        OutTexture t;
        t.name = name;
        t.rgba32 = pixel == 0;
        t.mode = mode;
        // TMEM holds 4 KB: 2048 16-bit texels, or 1024 32-bit ones (split over its two halves). Halve the longer side.
        int limit = (t.rgba32 ? 1024 : 2048) >> level, nw = w, nh = h;
        while (nw * nh > limit || nw > 256 || nh > 256) {
            if (nw >= nh) nw /= 2;
            else nh /= 2;
        }
        while (nw > max_w && nw > 1) nw /= 2;
        while (nh > max_h && nh > 1) nh /= 2;
        t.w = nw;
        t.h = nh;
        t.shrunk = nw != w || nh != h;
        t.tint = tint;
        std::vector<Rgba> small = (nw == w && nh == h) ? px : shrink(px, w, h, nw, nh);
        if (tint != 0xFFFFFF) {
            for (Rgba& p : small) {
                p.r = uint8_t(p.r * ((tint >> 16) & 0xFF) / 255);
                p.g = uint8_t(p.g * ((tint >> 8) & 0xFF) / 255);
                p.b = uint8_t(p.b * (tint & 0xFF) / 255);
            }
        }
        t.small = small;
        for (const Rgba& p : small) {
            if (t.rgba32) {
                t.data.insert(t.data.end(), { p.r, p.g, p.b, p.a });
            }
            else {
                uint16_t v = uint16_t((p.r >> 3) << 11 | (p.g >> 3) << 6 | (p.b >> 3) << 1 | (p.a >= 128 ? 1 : 0));
                t.data.push_back(uint8_t(v >> 8));
                t.data.push_back(uint8_t(v));
            }
        }
        return t;
    }

    // The load list (as 2049's: SETTIMG first, then the load, the tile and its size; texture animation reads the
    // first SETTIMG and the SETTILESIZE). SETTIMG is IMAG-relative, as in the N64's files.
    void add_load_list(Bytes& txld, const OutTexture& t) {
        if (t.ci8) {
            // As the N64's car lists: loaded as 16-bit texels, drawn CI 8-bit with the palette the game loads for the
            // car (its PLHD palette, recolored by src/car2049.cpp), TLUT left on as Rush 2 keeps it.
            uint32_t words = std::max<uint32_t>(1, t.w / 8);
            uint32_t dxt = (2048 + words - 1) / words;
            uint32_t lrs = (t.w * t.h + 1) / 2 - 1;
            uint32_t mask_s = log2i(t.w), mask_t = log2i(t.h);
            uint32_t list[] = {
                0xFD500000, t.texels,                                               // G_SETTIMG CI 16-bit
                0xF5500000, 0x07000000,                                             // G_SETTILE load tile
                0xE6000000, 0,                                                      // G_RDPLOADSYNC
                0xF3000000, 0x07000000 | lrs << 12 | dxt,                           // G_LOADBLOCK
                0xE7000000, 0,                                                      // G_RDPPIPESYNC
                0xD7000002, 0xFFFFFFFF,                                             // G_TEXTURE on, one level
                0xF5480000 | words << 9, mask_t << 14 | mask_s << 4,                // G_SETTILE render tile CI 8-bit
                0xF2000000, (uint32_t)(t.w - 1) << 14 | (uint32_t)(t.h - 1) << 2,   // G_SETTILESIZE
                0xE3001001, 0x00008000,                                             // texture LUT RGBA16
                0xDF000000, 0,
            };
            for (uint32_t v : list) add32(txld, v);
            return;
        }
        uint32_t siz = t.rgba32 ? 3 : 2, bytes = t.rgba32 ? 4 : 2;
        uint32_t words = std::max<uint32_t>(1, t.w * bytes / 8);
        uint32_t dxt = (2048 + words - 1) / words;
        uint32_t line = (t.w * 2 + 7) >> 3;
        uint32_t lrs = t.w * t.h - 1;
        uint32_t mask_s = log2i(t.w), mask_t = log2i(t.h);
        uint32_t list[] = {
            0xFD000000 | siz << 19, t.texels,                                   // G_SETTIMG RGBA, width 1
            0xF5000000 | siz << 19, 0x07000000,                                 // G_SETTILE load tile
            0xE6000000, 0,                                                      // G_RDPLOADSYNC
            0xF3000000, 0x07000000 | lrs << 12 | dxt,                           // G_LOADBLOCK
            0xE7000000, 0,                                                      // G_RDPPIPESYNC
            0xD7000002, 0xFFFFFFFF,                                             // G_TEXTURE on, one level
            0xF5000000 | siz << 19 | line << 9, mask_t << 14 | mask_s << 4,     // G_SETTILE render tile, wrapping
            0xF2000000, (uint32_t)(t.w - 1) << 14 | (uint32_t)(t.h - 1) << 2,   // G_SETTILESIZE
            0xE3001001, 0,                                                      // texture LUT off
            0xDF000000, 0,
        };
        for (uint32_t v : list) add32(txld, v);
    }

    // ------------------------------------------------------------------------------------------------------------
    // Object lists

    struct Vtx {
        int16_t x, y, z;
        int16_t s, t;
        uint8_t c[4];
        bool operator==(const Vtx& o) const {
            return x == o.x && y == o.y && z == o.z && s == o.s && t == o.t && memcmp(c, o.c, 4) == 0;
        }
    };

    struct SrcVtx {
        float p[3];
        float uv[2];
        uint8_t c[4];
    };

    // List commands in the making: pointer words are relative to the LOD's vertices or to its own list.
    enum class Fix { None, Vertices, List };
    struct Cmd {
        uint32_t w0, w1;
        Fix fix;
    };

    struct LodBuilder {
        const std::vector<OutTexture>& textures;
        uint32_t txld_base;         // file offset of TXLD
        uint16_t flags;             // final LOD flags
        int swap_texture;           // texture of a texture swap (its size sets the coordinates), or -1
        uint32_t swap_mode;         // render mode for it
        int bind_texture;           // texture loaded first where the disc takes one by code, or -1
        std::vector<Cmd> cmds;
        std::vector<Vtx> vertices;

        // Current batch
        std::vector<Vtx> batch;
        std::vector<std::array<uint8_t, 3>> tris;
        int batch_texture = -2;     // texture of the batch (-1 none)
        float shift_u = 0, shift_v = 0;
        // State the list has set
        int list_texture = -2;
        uint32_t list_mode = 0;
        const uint32_t* list_cc = nullptr;

        int effective(int tex) const {
            if (flags & 1) return swap_texture >= 0 ? swap_texture : -1;
            if (bind_texture >= 0) return bind_texture;
            return tex;
        }

        void flush() {
            if (tris.empty()) {
                batch.clear();
                return;
            }
            int tex = batch_texture;
            // State: the texture (unless the code supplies it), render mode and combiner.
            uint32_t mode;
            const uint32_t* cc;
            bool textured = tex >= 0 || (flags & 1);
            if (flags & 1) {
                mode = swap_mode ? swap_mode : tex >= 0 ? textures[tex].mode : rm_translucent;
            }
            else {
                mode = tex >= 0 ? textures[tex].mode : rm_opaque;
            }
            cc = textured ? cc_textured : cc_shade;
            if (!(flags & 1) && tex >= 0 && tex != list_texture) {
                cmds.push_back({ 0xDE000000, txld_base + textures[tex].list, Fix::None });
            }
            list_texture = tex;
            if (mode != list_mode) cmds.push_back({ 0xE200001C, mode, Fix::None });
            list_mode = mode;
            if (cc != list_cc) cmds.push_back({ cc[0], cc[1], Fix::None });
            list_cc = cc;
            uint32_t first = (uint32_t)vertices.size();
            uint32_t n = (uint32_t)batch.size();
            vertices.insert(vertices.end(), batch.begin(), batch.end());
            cmds.push_back({ 0x01000000 | n << 12 | n << 1, first * 16, Fix::Vertices });
            for (size_t i = 0; i < tris.size(); i += 2) {
                const auto& a = tris[i];
                uint32_t w0 = (uint32_t)a[0] * 2 << 16 | (uint32_t)a[1] * 2 << 8 | (uint32_t)a[2] * 2;
                if (i + 1 < tris.size()) {
                    const auto& b = tris[i + 1];
                    cmds.push_back({ 0x06000000 | w0, (uint32_t)b[0] * 2 << 16 | (uint32_t)b[1] * 2 << 8 | (uint32_t)b[2] * 2, Fix::None });
                }
                else {
                    cmds.push_back({ 0x05000000 | w0, 0, Fix::None });
                }
            }
            batch.clear();
            tris.clear();
        }

        // Forces the next batch to set its texture and modes again (after a block that may have been skipped).
        void forget_state() {
            list_texture = -2;
            list_mode = 0;
            list_cc = nullptr;
        }

        bool fits(float v, float shift, int size) const {
            double st = (v - shift) * size * 32.0 - 16.0;
            return st >= -32768.0 && st <= 32767.0;
        }

        Vtx make(const SrcVtx& v, int w, int h) const {
            Vtx o{};
            for (int k = 0; k < 3; k++) {
                double p = std::round(v.p[k] * 16.0);
                if (p < -32768.0 || p > 32767.0) throw Bad{ __LINE__ };
                (&o.x)[k] = (int16_t)p;
            }
            if (w > 0) {
                o.s = (int16_t)std::lround((v.uv[0] - shift_u) * w * 32.0 - 16.0);
                o.t = (int16_t)std::lround((v.uv[1] - shift_v) * h * 32.0 - 16.0);
            }
            memcpy(o.c, v.c, 4);
            return o;
        }

        void triangle(int stream_texture, const SrcVtx& a, const SrcVtx& b, const SrcVtx& c, int depth = 0) {
            int tex = effective(stream_texture);
            int w = tex >= 0 ? textures[tex].w : 0, h = tex >= 0 ? textures[tex].h : 0;
            const SrcVtx* v[3] = { &a, &b, &c };
            // A triangle spanning more repeats of its texture than the N64's 10.5 coordinates hold (+-1024 texels around
            // a whole repeat near its middle) is drawn as four, split at its edges' midpoints (exact: attributes are
            // linear across a triangle).
            if (w > 0) {
                float span_u = std::max({ a.uv[0], b.uv[0], c.uv[0] }) - std::min({ a.uv[0], b.uv[0], c.uv[0] });
                float span_v = std::max({ a.uv[1], b.uv[1], c.uv[1] }) - std::min({ a.uv[1], b.uv[1], c.uv[1] });
                if (!std::isfinite(span_u) || !std::isfinite(span_v)) throw Bad{ __LINE__ };
                if ((span_u + 2) * w > 2000 || (span_v + 2) * h > 2000) {
                    if (depth > 16) throw Bad{ __LINE__ };
                    auto mid = [](const SrcVtx& p, const SrcVtx& q) {
                        SrcVtx m;
                        for (int k = 0; k < 3; k++) m.p[k] = (p.p[k] + q.p[k]) / 2;
                        for (int k = 0; k < 2; k++) m.uv[k] = (p.uv[k] + q.uv[k]) / 2;
                        for (int k = 0; k < 4; k++) m.c[k] = uint8_t((p.c[k] + q.c[k] + 1) / 2);
                        return m;
                    };
                    SrcVtx ab = mid(a, b), bc = mid(b, c), ca = mid(c, a);
                    triangle(stream_texture, a, ab, ca, depth + 1);
                    triangle(stream_texture, ab, b, bc, depth + 1);
                    triangle(stream_texture, ca, bc, c, depth + 1);
                    triangle(stream_texture, ab, bc, ca, depth + 1);
                    return;
                }
            }
            for (int attempt = 0; attempt < 2; attempt++) {
                if (tex != batch_texture) {
                    flush();
                    batch_texture = tex;
                }
                if (tris.empty()) {
                    shift_u = std::floor((std::min({ a.uv[0], b.uv[0], c.uv[0] }) + std::max({ a.uv[0], b.uv[0], c.uv[0] })) / 2);
                    shift_v = std::floor((std::min({ a.uv[1], b.uv[1], c.uv[1] }) + std::max({ a.uv[1], b.uv[1], c.uv[1] })) / 2);
                }
                bool ok = true;
                if (w > 0) {
                    for (const SrcVtx* p : v) {
                        ok = ok && fits(p->uv[0], shift_u, w) && fits(p->uv[1], shift_v, h);
                    }
                }
                Vtx out[3];
                uint8_t idx[3];
                if (ok) {
                    std::vector<Vtx> added;
                    for (int k = 0; k < 3; k++) {
                        out[k] = make(*v[k], w, h);
                        auto it = std::find(batch.begin(), batch.end(), out[k]);
                        if (it == batch.end()) {
                            auto jt = std::find(added.begin(), added.end(), out[k]);
                            if (jt == added.end()) added.push_back(out[k]);
                        }
                    }
                    ok = batch.size() + added.size() <= 32;
                    if (ok) {
                        batch.insert(batch.end(), added.begin(), added.end());
                        for (int k = 0; k < 3; k++) {
                            idx[k] = (uint8_t)(std::find(batch.begin(), batch.end(), out[k]) - batch.begin());
                        }
                        tris.push_back({ idx[0], idx[1], idx[2] });
                        return;
                    }
                }
                if (tris.empty()) throw Bad{ __LINE__ };  // a single triangle out of range
                flush();
            }
            throw Bad{ __LINE__ };
        }
    };

    // One LOD's stream into a list and vertices appended to objs at file offset objs_base + objs.size(). Returns the
    // offsets of the list and of the vertices.
    // Disc textures a LOD's stream loads.
    void stream_textures(const DcModel& m, const DcLod& lod, std::vector<bool>& used) {
        const Bytes& d = *m.d;
        bool lit = (lod.flags & 0x10) != 0;
        size_t o = lod.stream;
        for (int guard = 0; guard < 4000000; guard++) {
            uint32_t w = le32(d, o);
            o += 4;
            uint32_t kind = w & 0xE0000000;
            if (kind == 0xE0000000) return;
            if (kind == 0xA0000000 || kind == 0x80000000) {
                uint16_t t = (uint16_t)w;
                if (kind == 0xA0000000 && t != 0xFFFF && t < used.size()) used[t] = true;
                o += 4;
                continue;
            }
            if (!(w & 0x10000000)) o += 12;
            if (!(w & 0x08000000)) o += 8;
            if (!(w & 0x04000000)) o += lit ? 12 : 4;
        }
        throw Bad{ __LINE__ };
    }

    // remap: disc texture index -> output texture index (-1 = not kept).
    std::pair<uint32_t, uint32_t> convert_lod(const DcModel& m, const DcLod& lod, LodBuilder& b, Bytes& objs, uint32_t objs_base,
                                              const std::vector<int>& remap) {
        const Bytes& d = *m.d;
        bool lit = (lod.flags & 0x10) != 0;
        SrcVtx cache_pos[256], cache_uv[256], cache_col[256];
        memset(cache_pos, 0, sizeof(cache_pos));
        memset(cache_uv, 0, sizeof(cache_uv));
        memset(cache_col, 0, sizeof(cache_col));
        std::vector<SrcVtx> strip;
        int texture = -1;
        // Conditional blocks: (stream offset where it ends, command index of the branch to patch).
        std::vector<std::pair<size_t, size_t>> blocks;
        size_t o = lod.stream;
        auto close_blocks = [&]() {
            while (!blocks.empty() && o >= blocks.back().first) {
                b.flush();
                b.cmds[blocks.back().second].w1 = (uint32_t)b.cmds.size() * 8;
                blocks.pop_back();
                b.forget_state();
            }
        };
        for (int guard = 0;; guard++) {
            if (guard > 4000000) throw Bad{ __LINE__ };
            close_blocks();
            uint32_t w = le32(d, o);
            o += 4;
            uint32_t kind = w & 0xE0000000;
            if (kind == 0xE0000000) break;
            if (kind == 0xA0000000) {
                uint16_t t = (uint16_t)w;
                if (t != 0xFFFF && t >= m.textures.size()) throw Bad{ __LINE__ };
                texture = t == 0xFFFF ? -1 : remap[t];
                o += 4;
                continue;
            }
            if (kind == 0x80000000) {
                uint32_t length = le32(d, o);
                o += 4;
                // Bit 0: drawn only when not mirrored, bit 1: only when mirrored. The N64's conditional op skips to its
                // target unless load flag bit 3 (not mirrored) or 4 (mirrored) is set.
                uint32_t cond = (w & 3) == 1 ? 3 : (w & 3) == 2 ? 4 : 0;
                if (cond && length) {
                    b.flush();
                    blocks.push_back({ o + length, b.cmds.size() });
                    b.cmds.push_back({ 0xE0010000 | cond, 0, Fix::List });
                    b.forget_state();
                }
                continue;
            }
            if (kind != 0 && kind != 0x20000000) throw Bad{ __LINE__ };
            uint32_t pi = (w >> 16) & 0xFF, ui = (w >> 8) & 0xFF, ci = w & 0xFF;
            if (!(w & 0x10000000)) {
                for (int k = 0; k < 3; k++) cache_pos[pi].p[k] = lef(d, o + k * 4);
                o += 12;
            }
            if (!(w & 0x08000000)) {
                // A few track strips carry non-numbers here (drawn by the PowerVR as some texel); take texel 0.
                for (int k = 0; k < 2; k++) {
                    float f = lef(d, o + k * 4);
                    cache_uv[ui].uv[k] = std::isfinite(f) && std::fabs(f) < 1e6f ? f : 0.0f;
                }
                o += 8;
            }
            if (!(w & 0x04000000)) {
                if (lit) {
                    // Normals, as the N64's lit vertices hold them: signed bytes in place of the color.
                    for (int k = 0; k < 3; k++) {
                        float n = std::clamp(lef(d, o + k * 4), -1.0f, 1.0f);
                        cache_col[ci].c[k] = (uint8_t)(int8_t)std::lround(n * 127.0f);
                    }
                    cache_col[ci].c[3] = 255;
                    o += 12;
                }
                else {
                    uint32_t argb = le32(d, o);
                    cache_col[ci].c[0] = uint8_t(argb >> 16);
                    cache_col[ci].c[1] = uint8_t(argb >> 8);
                    cache_col[ci].c[2] = uint8_t(argb);
                    cache_col[ci].c[3] = uint8_t(argb >> 24);
                    o += 4;
                }
            }
            SrcVtx v;
            memcpy(v.p, cache_pos[pi].p, sizeof(v.p));
            memcpy(v.uv, cache_uv[ui].uv, sizeof(v.uv));
            memcpy(v.c, cache_col[ci].c, 4);
            strip.push_back(v);
            if (kind == 0x20000000) {
                // Each vertex after the second makes a triangle, alternating in winding.
                for (size_t i = 2; i < strip.size(); i++) {
                    if (i % 2 == 0) b.triangle(texture, strip[i - 2], strip[i - 1], strip[i]);
                    else b.triangle(texture, strip[i - 1], strip[i - 2], strip[i]);
                }
                strip.clear();
            }
        }
        while (!blocks.empty()) {
            b.flush();
            b.cmds[blocks.back().second].w1 = (uint32_t)b.cmds.size() * 8;
            blocks.pop_back();
        }
        b.flush();
        // End: put back the texture LUT mode Rush 2 keeps (RGBA16), which the load lists turn off.
        b.cmds.push_back({ 0xE7000000, 0, Fix::None });
        b.cmds.push_back({ 0xE3001001, 0x00008000, Fix::None });
        b.cmds.push_back({ 0xDF000000, 0, Fix::None });

        // Vertices (flag halfword bit 15 chains them to the next), then the list.
        uint32_t vtx_at = objs_base + (uint32_t)objs.size();
        for (size_t i = 0; i < b.vertices.size(); i++) {
            const Vtx& v = b.vertices[i];
            size_t at = objs.size();
            objs.resize(at + 16);
            put16(objs, at, (uint16_t)v.x);
            put16(objs, at + 2, (uint16_t)v.y);
            put16(objs, at + 4, (uint16_t)v.z);
            put16(objs, at + 6, i + 1 < b.vertices.size() ? 0x8000 : 0);
            put16(objs, at + 8, (uint16_t)v.s);
            put16(objs, at + 10, (uint16_t)v.t);
            memcpy(&objs[at + 12], v.c, 4);
        }
        uint32_t list_at = objs_base + (uint32_t)objs.size();
        for (const Cmd& c : b.cmds) {
            uint32_t w1 = c.w1;
            if (c.fix == Fix::Vertices) w1 += vtx_at;
            if (c.fix == Fix::List) w1 += list_at;
            add32(objs, c.w0);
            add32(objs, w1);
        }
        return { list_at, b.vertices.empty() ? 0 : vtx_at };
    }

    // ------------------------------------------------------------------------------------------------------------

    std::string n64_object_name(const std::string& dc) {
        return rush2::rom2049::dc::n64_name(dc);
    }

    // 2049 looks objects up by binary search over 15 characters.
    bool name_less(const std::string& a, const std::string& b) {
        return strncmp(a.c_str(), b.c_str(), 15) < 0;
    }

    // ------------------------------------------------------------------------------------------------------------
    // Car paint

    uint16_t rgba5551(int r, int g, int b) {
        return uint16_t((std::clamp(r, 0, 255) >> 3) << 11 | (std::clamp(g, 0, 255) >> 3) << 6 | (std::clamp(b, 0, 255) >> 3) << 1 | 1);
    }

    // Up to `count` colors for the histogram `colors` (RGB555 -> texels), by median cut; `index` gets each color's.
    std::vector<uint16_t> median_cut(const std::unordered_map<uint16_t, uint32_t>& colors, size_t count,
                                     std::unordered_map<uint16_t, uint8_t>& index) {
        struct Box {
            std::vector<std::pair<uint16_t, uint32_t>> c;
        };
        auto channel = [](uint16_t v, int k) { return (v >> (10 - 5 * k)) & 31; };
        std::vector<Box> boxes(1);
        for (const auto& kv : colors) boxes[0].c.push_back(kv);
        while (boxes.size() < count) {
            // Split the box with the widest weighted range along that axis at its weighted median.
            int best = -1, axis = 0;
            uint64_t score = 0;
            for (size_t b = 0; b < boxes.size(); b++) {
                if (boxes[b].c.size() < 2) continue;
                uint64_t weight = 0;
                for (const auto& [v, n] : boxes[b].c) weight += n;
                for (int k = 0; k < 3; k++) {
                    int lo = 31, hi = 0;
                    for (const auto& [v, n] : boxes[b].c) {
                        lo = std::min(lo, channel(v, k));
                        hi = std::max(hi, channel(v, k));
                    }
                    uint64_t s = uint64_t(hi - lo) * (weight + 1);
                    if (hi > lo && s >= score) {
                        score = s;
                        best = (int)b;
                        axis = k;
                    }
                }
            }
            if (best < 0) break;
            auto& c = boxes[best].c;
            std::sort(c.begin(), c.end(), [&](const auto& a, const auto& b) { return channel(a.first, axis) < channel(b.first, axis); });
            uint64_t total = 0, run = 0;
            for (const auto& [v, n] : c) total += n;
            size_t cut = 1;
            for (; cut < c.size(); cut++) {
                run += c[cut - 1].second;
                if (run * 2 >= total) break;
            }
            cut = std::clamp<size_t>(cut, 1, c.size() - 1);
            Box upper;
            upper.c.assign(c.begin() + cut, c.end());
            c.resize(cut);
            boxes.push_back(std::move(upper));
        }
        std::vector<uint16_t> out;
        for (const Box& b : boxes) {
            uint64_t r = 0, g = 0, bl = 0, n = 0;
            for (const auto& [v, k] : b.c) {
                r += channel(v, 0) * k;
                g += channel(v, 1) * k;
                bl += channel(v, 2) * k;
                n += k;
                index[v] = (uint8_t)out.size();
            }
            n = std::max<uint64_t>(n, 1);
            out.push_back(uint16_t((r / n) << 11 | (g / n) << 6 | (bl / n) << 1 | 1));
        }
        return out;
    }

    // A disc car's paint as the N64's (src/car2049.cpp, §8 Paint): every texture CI8 over one 256-color palette whose
    // entries 32-63, 64-95 and 96-126 are the main, accent and stripe ramps (a color stepping towards black) the game
    // rewrites to the player's colors; 0 is clear, 128-255 the unpainted colors. The disc has no ramps: each paint job
    // (CARnPJ1-12) is a full set of the body textures colored for it. Texels whose hue differs between the jobs are
    // paint; the jobs' hues sort them into up to three regions (k-means over the hues in jobs 1-7, a region kept only
    // if it is a clean color, saturated or white, in most jobs), largest first; a texel's shade in its ramp is its
    // brightness against the region's in each job. Jobs 8 (chrome) and 9-12 (repeats) are left out.
    // Returns false (textures left as they are) if the jobs aren't there.
    bool paint_car(const rush2::rom2049::dc::Files& files, const std::string& dc_file, const DcModel& m, std::vector<OutTexture>& textures,
                   Bytes& palette) {
        constexpr int jobs = 7;
        // Each job's textures by the base texture's name (the job's less its color suffix, C1_TOP01_BLU).
        std::vector<std::map<std::string, std::vector<Rgba>>> job(jobs);
        for (int j = 0; j < jobs; j++) {
            Bytes data;
            if (!files.get(dc_file + "PJ" + std::to_string(j + 1) + ".LZS", data)) return false;
            DcModel pm = parse(data);
            for (const DcTexture& t : pm.textures) {
                size_t cut = t.name.rfind('_');
                if (cut != std::string::npos) job[j][t.name.substr(0, cut)] = decode(pm, t);
            }
        }

        // Each texture's texels in every job at the N64 size (empty when a job lacks it).
        struct Texel {
            int tex;
            uint32_t i;
            float hue[jobs * 3];
            float bright[jobs];
        };
        std::vector<std::vector<std::vector<Rgba>>> jobs_small(textures.size());
        std::vector<Texel> painted;
        std::vector<std::vector<int>> region(textures.size());
        for (size_t ti = 0; ti < textures.size(); ti++) {
            OutTexture& t = textures[ti];
            region[ti].assign(t.small.size(), -1);
            if (t.source < 0) continue;
            const DcTexture& base = m.textures[t.source];
            for (int j = 0; j < jobs; j++) {
                auto it = job[j].find(base.name);
                if (it == job[j].end() || it->second.size() != (size_t)base.w * base.h) {
                    jobs_small[ti].clear();
                    break;
                }
                const std::vector<Rgba>& px = it->second;
                jobs_small[ti].push_back(t.w == base.w && t.h == base.h ? px : shrink(px, base.w, base.h, t.w, t.h));
            }
            if (jobs_small[ti].size() != jobs) continue;
            for (uint32_t i = 0; i < t.small.size(); i++) {
                Texel x{};
                x.tex = (int)ti;
                x.i = i;
                float lo[3] = { 1, 1, 1 }, hi[3] = { 0, 0, 0 }, dim = 255;
                bool clear = false;
                for (int j = 0; j < jobs; j++) {
                    const Rgba& p = jobs_small[ti][j][i];
                    clear |= p.a < 128;
                    float mx = (float)std::max({ p.r, p.g, p.b });
                    x.bright[j] = mx;
                    dim = std::min(dim, mx);
                    const uint8_t c[3] = { p.r, p.g, p.b };
                    for (int k = 0; k < 3; k++) {
                        float h = c[k] / (mx + 8.0f);
                        x.hue[j * 3 + k] = h;
                        lo[k] = std::min(lo[k], h);
                        hi[k] = std::max(hi[k], h);
                    }
                }
                float spread = std::max({ hi[0] - lo[0], hi[1] - lo[1], hi[2] - lo[2] });
                if (!clear && spread >= 0.4f && dim >= 24) painted.push_back(x);
            }
        }

        // Regions.
        int regions = 0;
        std::vector<int> label(painted.size(), 0);
        std::vector<std::array<float, jobs * 3>> centers;
        auto distance = [](const float* a, const float* b) {
            float d = 0;
            for (int k = 0; k < jobs * 3; k++) d += (a[k] - b[k]) * (a[k] - b[k]);
            return d;
        };
        for (int k = std::min<int>(3, (int)painted.size()); k >= 1; k--) {
            centers.assign(k, {});
            // Seeds: the median, then each the texel farthest from those before.
            for (int c = 0; c < jobs * 3; c++) {
                std::vector<float> v(painted.size());
                for (size_t i = 0; i < painted.size(); i++) v[i] = painted[i].hue[c];
                std::nth_element(v.begin(), v.begin() + v.size() / 2, v.end());
                centers[0][c] = v[v.size() / 2];
            }
            for (int s = 1; s < k; s++) {
                float far = -1;
                size_t at = 0;
                for (size_t i = 0; i < painted.size(); i++) {
                    float d = 1e30f;
                    for (int c = 0; c < s; c++) d = std::min(d, distance(painted[i].hue, centers[c].data()));
                    if (d > far) {
                        far = d;
                        at = i;
                    }
                }
                std::copy(painted[at].hue, painted[at].hue + jobs * 3, centers[s].begin());
            }
            std::vector<size_t> sizes(k);
            for (int pass = 0; pass < 30; pass++) {
                std::vector<std::array<double, jobs * 3>> sum(k, std::array<double, jobs * 3>{});
                std::fill(sizes.begin(), sizes.end(), 0);
                for (size_t i = 0; i < painted.size(); i++) {
                    int best = 0;
                    float bd = 1e30f;
                    for (int c = 0; c < k; c++) {
                        float d = distance(painted[i].hue, centers[c].data());
                        if (d < bd) {
                            bd = d;
                            best = c;
                        }
                    }
                    label[i] = best;
                    sizes[best]++;
                    for (int q = 0; q < jobs * 3; q++) sum[best][q] += painted[i].hue[q];
                }
                for (int c = 0; c < k; c++) {
                    if (sizes[c] == 0) continue;
                    for (int q = 0; q < jobs * 3; q++) centers[c][q] = float(sum[c][q] / sizes[c]);
                }
            }
            // A region is a clean color (saturated, or white) in most jobs; mixes of regions and shading aren't.
            bool clean = true;
            for (int c = 0; c < k; c++) {
                int good = 0;
                for (int j = 0; j < jobs; j++) {
                    const float* h = &centers[c][j * 3];
                    float lo = std::min({ h[0], h[1], h[2] }), hi = std::max({ h[0], h[1], h[2] });
                    good += hi >= 0.85f && (lo <= 0.25f || lo >= 0.8f);
                }
                if (good < 5 || sizes[c] * 200 < painted.size()) clean = false;
            }
            regions = k;
            if (clean) break;
        }
        // Largest region first: main, accent, stripe.
        std::vector<int> order(regions), rank(regions);
        {
            std::vector<size_t> sizes(regions, 0);
            for (int l : label) sizes[l]++;
            for (int c = 0; c < regions; c++) order[c] = c;
            std::stable_sort(order.begin(), order.end(), [&](int a, int b) { return sizes[a] > sizes[b]; });
            for (int c = 0; c < regions; c++) rank[order[c]] = c;
        }
        // A region's brightness in each job: its 90th percentile.
        std::vector<std::array<float, jobs>> full(regions);
        for (int c = 0; c < regions; c++) {
            for (int j = 0; j < jobs; j++) {
                std::vector<float> v;
                for (size_t i = 0; i < painted.size(); i++) {
                    if (label[i] == c) v.push_back(painted[i].bright[j]);
                }
                if (v.empty()) {
                    full[c][j] = 255;
                    continue;
                }
                size_t at = v.size() * 9 / 10;
                std::nth_element(v.begin(), v.begin() + at, v.end());
                full[c][j] = std::max(v[at], 1.0f);
            }
        }
        constexpr int ramp_first[3] = { 32, 64, 96 };
        for (size_t i = 0; i < painted.size(); i++) {
            const Texel& x = painted[i];
            int c = label[i];
            float shade[jobs];
            for (int j = 0; j < jobs; j++) shade[j] = x.bright[j] / full[c][j];
            std::nth_element(shade, shade + jobs / 2, shade + jobs);
            int r = rank[c];
            int step = std::clamp((int)std::lround((1.0f - shade[jobs / 2]) * 32.0f), 0, r == 2 ? 29 : 30);
            region[x.tex][x.i] = ramp_first[r] + 1 + step;
        }

        // The palette: the regions' colors in the first job, then the unpainted colors.
        std::vector<uint16_t> pal(256, 0);
        for (int c = 0; c < regions; c++) {
            const float* h = &centers[c][0];
            float scale = full[c][0] / std::max({ h[0], h[1], h[2], 0.01f });
            int first = ramp_first[rank[c]];
            for (int s = 0; s <= 31 && first + s < 128; s++) {
                float t = s == 0 ? 1.0f : 1.0f - (s - 1) / 32.0f;
                pal[first + s] = rgba5551(int(h[0] * scale * t), int(h[1] * scale * t), int(h[2] * scale * t));
            }
        }
        auto unpainted = [&](size_t ti, uint32_t i) -> const Rgba& {
            return jobs_small[ti].empty() ? textures[ti].small[i] : jobs_small[ti][0][i];
        };
        std::unordered_map<uint16_t, uint32_t> histogram;
        for (size_t ti = 0; ti < textures.size(); ti++) {
            for (uint32_t i = 0; i < textures[ti].small.size(); i++) {
                const Rgba& p = unpainted(ti, i);
                if (region[ti][i] < 0 && p.a >= 128) histogram[rgba5551(p.r, p.g, p.b) >> 1]++;
            }
        }
        std::unordered_map<uint16_t, uint8_t> index;
        std::vector<uint16_t> rest = median_cut(histogram, 128, index);
        for (size_t c = 0; c < rest.size(); c++) pal[128 + c] = rest[c];
        for (size_t ti = 0; ti < textures.size(); ti++) {
            OutTexture& t = textures[ti];
            t.data.resize(t.small.size());
            for (uint32_t i = 0; i < t.small.size(); i++) {
                const Rgba& p = unpainted(ti, i);
                t.data[i] = region[ti][i] >= 0 ? (uint8_t)region[ti][i]
                          : p.a < 128      ? 0
                                           : uint8_t(128 + index[rgba5551(p.r, p.g, p.b) >> 1]);
            }
            t.ci8 = true;
            t.rgba32 = false;
            t.shrunk = false; // the full-size image isn't paintable: drawn as converted
        }
        palette.clear();
        for (uint16_t c : pal) {
            palette.push_back(uint8_t(c >> 8));
            palette.push_back(uint8_t(c));
        }
        return true;
    }
}

std::string rush2::rom2049::dc::n64_name(const std::string& dc_name) {
    static const std::unordered_map<std::string, std::string> map = [] {
        std::unordered_map<std::string, std::string> m;
        for (const Rename& r : renames) m[r.dc] = r.n64;
        return m;
    }();
    auto it = map.find(dc_name);
    return it == map.end() ? dc_name : it->second;
}

bool rush2::rom2049::dc::decode_texture(const std::vector<uint8_t>& container, uint32_t index, uint32_t tint,
                                        std::vector<uint8_t>& rgba, int& w, int& h) {
    try {
        DcModel m = parse(container);
        if (index >= m.textures.size()) return false;
        const DcTexture& t = m.textures[index];
        std::vector<Rgba> px = decode(m, t);
        w = t.w;
        h = t.h;
        rgba.resize(px.size() * 4);
        for (size_t i = 0; i < px.size(); i++) {
            rgba[i * 4] = uint8_t(px[i].r * ((tint >> 16) & 0xFF) / 255);
            rgba[i * 4 + 1] = uint8_t(px[i].g * ((tint >> 8) & 0xFF) / 255);
            rgba[i * 4 + 2] = uint8_t(px[i].b * (tint & 0xFF) / 255);
            rgba[i * 4 + 3] = px[i].a;
        }
        return true;
    }
    catch (const Bad&) {
        return false;
    }
}

bool rush2::rom2049::dc::convert_model(const Files& files, const std::string& dc_file, int n64_file, std::vector<uint8_t>& out,
                                       std::vector<SourceTexture>* shrunk) {
    Bytes d;
    if (!files.get(dc_file + ".LZS", d)) return false;
    try {
        DcModel m = parse(d);

        // Objects: (output name, disc object, overrides).
        struct Plan {
            std::string name;
            const DcObject* src;
            const FileObject* spec;
        };
        std::vector<Plan> plan;
        bool listed = false;
        for (const FileObject& f : file_objects) {
            if (f.file != n64_file) continue;
            listed = true;
            for (const DcObject& o : m.objects) {
                if (o.name == f.dc) {
                    plan.push_back({ f.n64, &o, &f });
                    break;
                }
            }
        }
        if (!listed) {
            for (const DcObject& o : m.objects) plan.push_back({ n64_object_name(o.name), &o, nullptr });
        }
        std::stable_sort(plan.begin(), plan.end(), [](const Plan& a, const Plan& b) { return name_less(a.name, b.name); });

        // Textures. Track geometry keeps all the disc's. Other files keep those their objects load and those the N64
        // file names (no bigger than the N64's), in the disc's order, then the N64's tinted copies.
        std::vector<bool> used(m.textures.size(), !listed);
        std::map<std::string, std::pair<int, int>> caps;
        for (const NamedTexture& t : named_textures) {
            if (t.file == n64_file) caps[t.name] = { t.w, t.h };
        }
        for (size_t i = 0; i < m.textures.size(); i++) {
            if (caps.contains(m.textures[i].name)) used[i] = true;
        }
        for (const Plan& p : plan) {
            for (int j = 0; j < p.src->lod_count; j++) {
                const DcLod& l = p.src->lods[j];
                stream_textures(m, l, used);
                if ((l.flags & 1) && l.handle < used.size()) used[l.handle] = true;
            }
        }
        std::vector<std::vector<Rgba>> pixels(m.textures.size());
        for (size_t i = 0; i < m.textures.size(); i++) {
            if (used[i]) pixels[i] = decode(m, m.textures[i]);
        }
        uint32_t budget = 0;
        for (const FileSize& f : file_sizes) {
            if (f.file == n64_file) budget = f.bytes;
        }
        // Over the N64 file's size, textures go down a size at a time (the full-size ones replace them on screen).
        for (int level = 0;; level++) {
        std::vector<OutTexture> textures;
        std::vector<int> remap(m.textures.size(), -1);
        for (size_t i = 0; i < m.textures.size(); i++) {
            if (!used[i]) continue;
            const DcTexture& t = m.textures[i];
            auto cap = caps.find(t.name);
            remap[i] = (int)textures.size();
            textures.push_back(make_texture(t.name, pixels[i], t.w, t.h, t.pixel, 0xFFFFFF, level,
                                            cap == caps.end() ? 256 : cap->second.first,
                                            cap == caps.end() ? 256 : cap->second.second,
                                            texture_mode(n64_file, t.name, pixels[i])));
            textures.back().source = (int)i;
        }
        for (const Tinted& t : tinted) {
            if (t.file != n64_file) continue;
            for (size_t i = 0; i < m.textures.size(); i++) {
                const DcTexture& s = m.textures[i];
                if (s.name == t.dc) {
                    auto cap = caps.find(t.n64);
                    if (pixels[i].empty()) pixels[i] = decode(m, s);
                    textures.push_back(make_texture(t.n64, pixels[i], s.w, s.h, s.pixel, t.rgb, level,
                                                    cap == caps.end() ? 256 : cap->second.first,
                                                    cap == caps.end() ? 256 : cap->second.second,
                                                    texture_mode(n64_file, t.n64, pixels[i])));
                    textures.back().source = (int)i;
                    break;
                }
            }
        }
        auto texture_index = [&](const char* name) {
            for (size_t i = 0; i < textures.size(); i++) {
                if (textures[i].name == name) return (int)i;
            }
            return -1;
        };

        // Cars (files 88-100) are painted through a palette, as the N64's.
        Bytes palette;
        if (n64_file >= 88 && n64_file <= 100 && !paint_car(files, dc_file, m, textures, palette)) palette.clear();

        // Layout: 8-byte header, IMAG (the palette first), TXLD, OBHD, PLHD, TXHD, OBJS, PATH, PTHD, chunk directory.
        Bytes imag = palette;
        for (OutTexture& t : textures) {
            t.texels = (uint32_t)imag.size();
            imag.insert(imag.end(), t.data.begin(), t.data.end());
            align8(imag);
        }
        Bytes txld;
        for (OutTexture& t : textures) {
            t.list = (uint32_t)txld.size();
            add_load_list(txld, t);
        }
        // An 8-byte gap after IMAG and TXLD: track2049's merge_models maps a chunk's end address into that chunk, so
        // the first load list must not start where the texels end.
        uint32_t imag_at = 8;
        uint32_t txld_at = imag_at + (uint32_t)imag.size() + 8;
        uint32_t obhd_at = txld_at + (uint32_t)txld.size() + 8;
        uint32_t palettes = palette.empty() ? 0 : 1;
        uint32_t plhd_at = obhd_at + (uint32_t)plan.size() * 0x58;
        uint32_t txhd_at = plhd_at + palettes * 0x18;
        uint32_t objs_at = txhd_at + (uint32_t)textures.size() * 0x24;
        objs_at = (objs_at + 7) & ~7u;

        Bytes obhd(plan.size() * 0x58, 0), objs;
        for (size_t i = 0; i < plan.size(); i++) {
            const Plan& p = plan[i];
            size_t r = i * 0x58;
            memcpy(&obhd[r], p.name.data(), std::min<size_t>(p.name.size(), 16));
            put32(obhd, r + 16, p.src->radius);
            put16(obhd, r + 20, p.src->kind);
            put16(obhd, r + 22, (uint16_t)p.src->lod_count);
            for (int j = 0; j < p.src->lod_count; j++) {
                const DcLod& l = p.src->lods[j];
                uint16_t flags = l.flags;
                int handle = l.handle < remap.size() && remap[l.handle] >= 0 ? remap[l.handle] : 0;
                int swap = (flags & 1) && l.handle < remap.size() ? remap[l.handle] : -1;
                uint32_t swap_mode = 0;
                int bind = -1;
                if (p.spec) {
                    if (p.spec->flags >= 0) flags = (uint16_t)p.spec->flags;
                    if (*p.spec->handle) {
                        int h = texture_index(p.spec->handle);
                        if (h >= 0) handle = swap = h;
                    }
                    if (*p.spec->bind) bind = texture_index(p.spec->bind);
                    swap_mode = p.spec->mode;
                    if (!(flags & 1)) swap = -1;
                }
                if (swap >= (int)textures.size()) swap = -1;
                LodBuilder b{ textures, txld_at, flags, swap, swap_mode, bind };
                align8(objs);
                auto [list, vertices] = convert_lod(m, l, b, objs, objs_at, remap);
                size_t e = r + 0x18 + j * 16;
                put16(obhd, e, (uint16_t)handle);
                put16(obhd, e + 2, flags);
                put32(obhd, e + 4, l.distance);
                put32(obhd, e + 8, list);
                put32(obhd, e + 12, vertices);
            }
        }

        Bytes txhd(textures.size() * 0x24, 0);
        for (size_t i = 0; i < textures.size(); i++) {
            const OutTexture& t = textures[i];
            size_t r = i * 0x24;
            memcpy(&txhd[r], t.name.data(), std::min<size_t>(t.name.size(), 16));
            put16(txhd, r + 16, (uint16_t)t.w);
            put16(txhd, r + 18, (uint16_t)t.h);
            txhd[r + 20] = t.ci8 ? 2 : 0;           // CI or RGBA
            txhd[r + 21] = t.ci8 ? 1 : t.rgba32 ? 3 : 2;
            put16(txhd, r + 22, 0xFFFF);            // no palette
            put32(txhd, r + 24, (txld_at - imag_at) + t.list);  // load list, IMAG-relative
            put32(txhd, r + 28, 0);                 // flag 0x08000000 clear: the record names a load list
        }

        // Track geometry files carry the movers' paths: PATH nodes (16 floats and a word, all 32-bit) and PTHD headers
        // {name[16], u32 flags, s16 nodes, s16 trigger, u32 link, u32 nodes (file offset), s32 dynamic id}.
        Bytes path, pthd;
        uint32_t path_at = 0, pthd_at = 0, pthd_count = 0, dc_path = 0;
        bool has_paths = m.chunks.contains("PATH") && m.chunks.contains("PTHD");
        if (has_paths) {
            auto [po, ps] = m.chunks["PATH"];
            auto [ho, hn] = m.chunks["PTHD"];
            if ((uint64_t)po + ps > d.size() || (uint64_t)ho + (uint64_t)hn * 36 > d.size()) throw Bad{ __LINE__ };
            dc_path = po;
            for (uint32_t k = 0; k + 4 <= ps; k += 4) add32(path, le32(d, po + k));
            pthd_count = hn;
            pthd.resize((size_t)hn * 36);
            for (uint32_t k = 0; k < hn; k++) {
                size_t s = ho + k * 36, r = (size_t)k * 36;
                memcpy(&pthd[r], &d[s], 16);
                put32(pthd, r + 16, le32(d, s + 16));
                put16(pthd, r + 20, le16(d, s + 20));
                put16(pthd, r + 22, le16(d, s + 22));
                put32(pthd, r + 24, le32(d, s + 24));
                put32(pthd, r + 28, le32(d, s + 28));   // relocated below
                put32(pthd, r + 32, le32(d, s + 32));
            }
        }

        out.assign(8, 0);
        auto place = [&](const Bytes& b) {
            uint32_t at = (uint32_t)out.size();
            out.insert(out.end(), b.begin(), b.end());
            return at;
        };
        place(imag);
        out.resize(out.size() + 8, 0);
        place(txld);
        out.resize(out.size() + 8, 0);
        place(obhd);
        if (palettes) {
            // {name[16], 0x00FF8000 as the N64's car palettes, data (IMAG-relative)}
            Bytes plhd(0x18, 0);
            std::string name = "C" + dc_file.substr(3) + "PALETTE";
            memcpy(&plhd[0], name.data(), std::min<size_t>(name.size(), 15));
            put32(plhd, 16, 0x00FF8000);
            put32(plhd, 20, 0);
            place(plhd);
        }
        place(txhd);
        align8(out);
        if (out.size() != objs_at) throw Bad{ __LINE__ };
        place(objs);
        align8(out);
        if (has_paths) {
            path_at = place(path);
            align8(out);
            for (uint32_t k = 0; k < pthd_count; k++) {
                size_t r = (size_t)k * 36 + 28;
                uint32_t nodes = (pthd[r] << 24) | (pthd[r + 1] << 16) | (pthd[r + 2] << 8) | pthd[r + 3];
                put32(pthd, r, nodes - dc_path + path_at);
            }
            pthd_at = place(pthd);
            align8(out);
        }
        struct Entry {
            const char* tag;
            uint32_t at, n;
        };
        std::vector<Entry> dir = {
            { "IMAG", imag_at, (uint32_t)imag.size() }, { "TXLD", txld_at, (uint32_t)txld.size() },
            { "OBHD", obhd_at, (uint32_t)plan.size() }, { "PLHD", plhd_at, palettes },
            { "TXHD", txhd_at, (uint32_t)textures.size() }, { "OBJS", objs_at, (uint32_t)objs.size() },
        };
        if (has_paths) {
            dir.push_back({ "PATH", path_at, (uint32_t)path.size() });
            dir.push_back({ "PTHD", pthd_at, pthd_count });
        }
        put32(out, 0, (uint32_t)out.size());
        put32(out, 4, (uint32_t)dir.size());
        for (const Entry& e : dir) {
            out.insert(out.end(), e.tag, e.tag + 4);
            add32(out, e.at);
            add32(out, e.n);
        }
        if (budget && out.size() > budget) {
            if (level < 5) continue;
            fprintf(stderr, "rush2049 dc: %s for N64 file %d is %zu bytes, over the N64's %u\n", dc_file.c_str(), n64_file,
                    out.size(), budget);
        }
        if (shrunk != nullptr) {
            for (const OutTexture& t : textures) {
                if (!t.shrunk || t.source < 0) continue;
                SourceTexture st;
                st.w = (uint16_t)t.w;
                st.h = (uint16_t)t.h;
                st.rgba32 = t.rgba32;
                st.texels = t.data;
                st.file = dc_file;
                st.index = (uint32_t)t.source;
                st.tint = t.tint;
                shrunk->push_back(std::move(st));
            }
        }
        return true;
        }
    }
    catch (const Bad& e) {
        fprintf(stderr, "rush2049 dc: %s for N64 file %d doesn't convert (rush2049_dc_model.cpp:%d)\n", dc_file.c_str(), n64_file, e.line);
        out.clear();
        return false;
    }
}
