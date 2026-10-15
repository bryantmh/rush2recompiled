// Reading Rush 2 model containers into named textures and triangles, and writing them as glTF (include/asset_catalog.h,
// docs/asset_catalog.md). Nothing here touches the game, so it builds on its own for testing
// (tools/asset_catalog_test.cpp).
//
// The lists are run as the RSP and RDP would run them: G_DL calls into the texture-load lists, the conditional op
// (geometry.md 4.1) taken as the game takes it for one player, unmirrored, and every RDP texture command applied to a
// copy of TMEM with RT64's own load code (RDP::loadTileOperation / loadBlockOperation / loadTLUTOperation in
// lib/rt64/src/hle/rt64_rdp.cpp, minus the byte swap of its RDRAM). At each triangle the render tile is hashed with
// RT64's TMEMHasher and decoded with decode_tmem, so a texture's hash, image and content key are the ones the game's
// draw gives RT64 and the texture observer (src/texture_upscale.cpp).

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstring>
#include <map>
#include <set>
#include <unordered_map>

#define XXH_INLINE_ALL
#include "xxHash/xxhash.h"
#include "shared/rt64_f3d_defines.h"
#include "common/rt64_load_types.h"
#include "common/rt64_tmem_hasher.h"

#include "asset_catalog.h"

namespace rush2::catalog {

namespace {
    constexpr uint32_t tmem_bytes = 4096;
    constexpr uint32_t tmem_mask8 = 0xFFF, tmem_mask16 = 0x7FF;
    constexpr uint32_t no_address = 0xFFFFFFFF;
    constexpr uint32_t texel_record = 0x08000000; // Texture record flag: data is texels, not a texture-load list.
    // Load flags the conditional op tests (geometry.md 4.1): bit 1 one player, bit 3 not mirrored.
#ifdef CATALOG_LOAD_FLAGS
    constexpr uint32_t load_flags = CATALOG_LOAD_FLAGS;   // tools/asset_catalog_test.bat: other branch variants.
#else
    constexpr uint32_t load_flags = (1u << 1) | (1u << 3);
#endif

    uint32_t be32(const std::vector<uint8_t>& d, size_t o) {
        return o + 4 <= d.size() ? (uint32_t(d[o]) << 24) | (uint32_t(d[o + 1]) << 16) | (uint32_t(d[o + 2]) << 8) | d[o + 3] : 0;
    }
    uint16_t be16(const std::vector<uint8_t>& d, size_t o) {
        return o + 2 <= d.size() ? uint16_t((d[o] << 8) | d[o + 1]) : 0;
    }
    float bef(const std::vector<uint8_t>& d, size_t o) {
        uint32_t v = be32(d, o);
        float f;
        memcpy(&f, &v, 4);
        return f;
    }
    std::string cstr(const std::vector<uint8_t>& d, size_t o, size_t n) {
        std::string s;
        for (size_t i = 0; i < n && o + i < d.size() && d[o + i] != 0; i++) {
            s += char(d[o + i]);
        }
        return s;
    }

    Edge edge_of(uint8_t cm, uint8_t mask) {
        if ((cm & 2) || mask == 0) {
            return Edge::Clamp;
        }
        return (cm & 1) ? Edge::Mirror : Edge::Wrap;
    }

    // RDP state: TMEM, the tiles, the texture image, and which load filled each TMEM word.
    struct Rdp {
        const std::vector<uint8_t>& file;
        uint8_t tmem[tmem_bytes] = {};
        RT64::LoadTile tiles[8] = {};
        RT64::LoadTexture image = {};
        uint32_t loaded_from[tmem_bytes / 8]; // Texel address of the load that started at each TMEM word.

        explicit Rdp(const std::vector<uint8_t>& f) : file(f) {
            std::fill(std::begin(loaded_from), std::end(loaded_from), no_address);
        }

        uint8_t byte(uint32_t address) const { return address < file.size() ? file[address] : 0; }

        // RT64's loadWord, reading big-endian file bytes (RT64's RDRAM is byte-swapped in words, hence its ^3).
        void load_word(bool rgba32, bool tlut, uint32_t tmem_address, uint32_t xor_mask, uint32_t texture_address) {
            uint32_t offset_mask = tlut ? 0x1 : 0x7;
            auto b = [&](uint32_t i) { return byte(texture_address + (i & offset_mask)); };
            if (rgba32) {
                const uint32_t upper = tmem_bytes >> 1;
                tmem[((tmem_address + 0) ^ xor_mask) & tmem_mask8] = b(0);
                tmem[((tmem_address + 1) ^ xor_mask) & tmem_mask8] = b(1);
                tmem[((tmem_address + 2) ^ xor_mask) & tmem_mask8] = b(4);
                tmem[((tmem_address + 3) ^ xor_mask) & tmem_mask8] = b(5);
                tmem[(((tmem_address + 0) ^ xor_mask) | upper) & tmem_mask8] = b(2);
                tmem[(((tmem_address + 1) ^ xor_mask) | upper) & tmem_mask8] = b(3);
                tmem[(((tmem_address + 2) ^ xor_mask) | upper) & tmem_mask8] = b(6);
                tmem[(((tmem_address + 3) ^ xor_mask) | upper) & tmem_mask8] = b(7);
            }
            else {
                for (uint32_t i = 0; i < 8; i++) {
                    tmem[((tmem_address + i) ^ xor_mask) & tmem_mask8] = b(i);
                }
            }
        }

        // RT64's loadToTMEMCommon.
        void load_common(bool rgba32, bool block, bool tlut, uint32_t texture_start, uint32_t texture_stride,
                         uint32_t tmem_start, uint32_t tmem_stride, uint32_t words_per_row, uint32_t row_count,
                         uint32_t dxt_increment = 0) {
            const uint32_t dxt_swap = 0x800;
            uint32_t texture_address, tmem_address, word_count;
            const uint32_t tmem_mask = rgba32 ? tmem_mask16 : tmem_mask8;
            const uint32_t tmem_advance = rgba32 ? 0x4 : 0x8;
            const uint32_t texture_advance = tlut ? 0x2 : 0x8;
            uint32_t xor_mask = 0, dxt_counter = 0;
            uint32_t texture_row = texture_start;
            uint32_t tmem_row = tmem_start & tmem_mask;
            if (tmem_stride > 0 && (words_per_row * tmem_advance) <= tmem_stride) {
                int32_t skip = int32_t(row_count) - int32_t((tmem_mask + tmem_stride) / tmem_stride);
                if (skip > 0) {
                    tmem_row += (tmem_row + tmem_stride * skip) & tmem_mask;
                    texture_row += texture_stride * skip;
                    xor_mask = (skip & 0x1) << 2;
                    row_count -= skip;
                }
            }
            while (row_count > 0) {
                texture_address = texture_row;
                tmem_address = tmem_row;
                word_count = words_per_row;
                while (word_count > 0) {
                    load_word(rgba32, tlut, tmem_address, xor_mask, texture_address);
                    if (block) {
                        dxt_counter += dxt_increment;
                        while (dxt_counter >= dxt_swap) {
                            tmem_address = (tmem_address + tmem_stride) & tmem_mask;
                            dxt_counter -= dxt_swap;
                            xor_mask ^= 0x4;
                        }
                    }
                    texture_address += texture_advance;
                    tmem_address = (tmem_address + tmem_advance) & tmem_mask;
                    word_count--;
                }
                tmem_row = (tmem_row + tmem_stride) & tmem_mask;
                texture_row += texture_stride;
                xor_mask ^= 0x4;
                row_count--;
            }
        }

        static bool is_rgba32(const RT64::LoadTile& t) { return t.siz == G_IM_SIZ_32b && t.fmt == G_IM_FMT_RGBA; }

        void load_tile(uint8_t tile, uint16_t uls, uint16_t ult, uint16_t lrs, uint16_t lrt) {
            auto& t = tiles[tile & 7];
            t.uls = uls; t.ult = ult; t.lrs = lrs; t.lrt = lrt;
            if (t.uls > t.lrs) {
                return;
            }
            const uint32_t bytes_offset = (t.uls >> 2) << image.siz >> 1;
            const uint32_t bytes_per_row = image.width << image.siz >> 1;
            const uint32_t texture_start = image.address + bytes_offset + bytes_per_row * (t.ult >> 2);
            const uint32_t row_count = 1 + ((t.lrt >> 2) - (t.ult >> 2));
            const uint32_t words_per_row = (((t.lrs >> 2) - (t.uls >> 2)) >> (4 - t.siz)) + 1;
            load_common(is_rgba32(t), false, false, texture_start, bytes_per_row, t.tmem << 3, t.line << 3,
                        words_per_row, row_count);
            loaded_from[t.tmem & 0x1FF] = image.address;
        }

        void load_block(uint8_t tile, uint16_t uls, uint16_t ult, uint16_t lrs, uint16_t dxt) {
            auto& t = tiles[tile & 7];
            t.uls = uls; t.ult = ult; t.lrs = lrs; t.lrt = dxt;
            if (t.uls > t.lrs || t.lrs >= 0x800) {
                return;
            }
            const uint32_t bytes_offset = t.uls << image.siz >> 1;
            const uint32_t bytes_per_row = image.width << image.siz >> 1;
            const uint32_t texture_start = image.address + bytes_offset + bytes_per_row * t.ult;
            const uint32_t word_count = ((t.lrs - t.uls) >> (4 - t.siz)) + 1;
            load_common(is_rgba32(t), true, false, texture_start, bytes_per_row, t.tmem << 3, t.line << 3, word_count,
                        1, t.lrt);
            loaded_from[t.tmem & 0x1FF] = image.address;
        }

        void load_tlut(uint8_t tile, uint16_t uls, uint16_t ult, uint16_t lrs, uint16_t lrt) {
            auto& t = tiles[tile & 7];
            t.uls = uls; t.ult = ult; t.lrs = lrs; t.lrt = lrt;
            const uint32_t bytes_offset = (t.uls >> 2) << image.siz >> 1;
            const uint32_t bytes_per_row = image.width << image.siz >> 1;
            const uint32_t texture_start = image.address + bytes_offset + bytes_per_row * (t.ult >> 2);
            const uint32_t row_count = 1 + ((t.lrt >> 2) - (t.ult >> 2));
            const uint32_t words_per_row = ((t.lrs >> 2) - (t.uls >> 2)) + 1;
            load_common(is_rgba32(t), false, true, texture_start, bytes_per_row, t.tmem << 3, t.line << 5,
                        words_per_row, row_count);
            loaded_from[t.tmem & 0x1FF] = image.address;
        }
    };

    // The size RT64 samples a tile at (State::fullSyncFramebufferPairTiles): the tile's size where it clamps, its
    // mask where it wraps, the smaller of the two.
    void sample_size(const RT64::LoadTile& tile, uint16_t& width, uint16_t& height) {
        const bool clamp_s = (tile.masks == 0) || (tile.cms & G_TX_CLAMP);
        const bool clamp_t = (tile.maskt == 0) || (tile.cmt & G_TX_CLAMP);
        const int32_t tile_width = clamp_s ? std::max((tile.lrs - tile.uls + 4) / 4, 1) : 0xFFFF;
        const int32_t tile_height = clamp_t ? std::max((tile.lrt - tile.ult + 4) / 4, 1) : 0xFFFF;
        const int32_t mask_width = (tile.masks > 0) ? int32_t(1 << tile.masks) : 0xFFFF;
        const int32_t mask_height = (tile.maskt > 0) ? int32_t(1 << tile.maskt) : 0xFFFF;
        width = uint16_t(std::min(tile_width, mask_width));
        height = uint16_t(std::min(tile_height, mask_height));
    }

    float shift_scale(uint8_t shift) {
        if (shift == 0) return 1.0f;
        if (shift <= 10) return 1.0f / float(1 << shift);
        return float(1 << (16 - shift));
    }

    // Fills TMEM's palette area (from 16-color palette `palette` for CI4) with a gray RGBA16 ramp of count colors.
    void gray_palette(uint8_t* tmem, uint32_t count, uint32_t palette) {
        uint32_t base = 0x800 + (count == 16 ? (palette << 7) : 0);
        for (uint32_t i = 0; i < count; i++) {
            uint32_t g = count == 16 ? i * 31 / 15 : (i & 31); // Rush 2's paint palettes are 32-color ramps.
            uint16_t v = uint16_t((g << 11) | (g << 6) | (g << 1) | 1);
            for (uint32_t k = 0; k < 4; k++) {
                tmem[(base + i * 8 + k * 2) & 0xFFF] = uint8_t(v >> 8);
                tmem[(base + i * 8 + k * 2 + 1) & 0xFFF] = uint8_t(v);
            }
        }
    }

    // Whether a 2-cycle combiner reads TEXEL1 (RT64's ColorCombiner::usesTexture): then RT64 samples the tile after
    // the render tile too.
    bool uses_texel1(uint64_t combine) {
        uint32_t w0 = uint32_t(combine >> 32), w1 = uint32_t(combine);
        uint32_t color_abd[] = { (w0 >> 20) & 0xF, (w0 >> 5) & 0xF, w1 >> 28, (w1 >> 24) & 0xF, (w1 >> 15) & 7, (w1 >> 6) & 7 };
        uint32_t color_c[] = { (w0 >> 15) & 0x1F, w0 & 0x1F };
        uint32_t alpha[] = { (w0 >> 12) & 7, (w0 >> 9) & 7, (w1 >> 21) & 7, (w1 >> 18) & 7, (w1 >> 12) & 7, (w1 >> 9) & 7,
                             (w1 >> 3) & 7, w1 & 7 };
        for (uint32_t v : color_abd) if (v == 2) return true;
        for (uint32_t v : color_c) if (v == 2 || v == 9) return true;
        for (uint32_t v : alpha) if (v == 2) return true;
        return false;
    }

    // A texture as drawn: what Walker::texture looks up its Container::textures index by.
    struct DrawnTexture {
        int index;
        float scale_s, scale_t;   // Texels per S10.5 unit after the TEXTURE scale and the tile shift.
        float offset_s, offset_t; // Texels subtracted (the tile's uls/ult).
        float width, height;
    };

    class Walker {
    public:
        Walker(const std::vector<uint8_t>& f, Container& c, bool g) : file(f), out(c), geometry(g), rdp(f) {}

        // G_SETTIMG addresses read from elsewhere instead (copies: a 2049 car's scuffed panels).
        std::map<uint32_t, uint32_t> redirect;
        // Texel addresses each texture record names (its data, or the SETTIMGs of its load list), for naming.
        std::map<uint32_t, std::string> record_names;
        std::vector<int> mip_of;        // Per texture: the level-0 texture it's a mipmap of, or -1.
        std::vector<int> mip_level;
        std::vector<uint32_t> palette_from; // Per texture: the address its TLUT was loaded from (no_address: none).
        // The palette record a painted texture is shown with (SF Rush's cars: the RED one), else no_address (gray).
        uint32_t paint_record = no_address;

        void run_lod(const std::string& model, uint32_t list, Lod* lod) {
            reset();
            current_model = model;
            current_lod = lod;
            run(list);
        }

        // The palette load func_80078190 adds after a record's texture when its +0x16 names a palette record: entries
        // first..last (record +0x10, +0x11) into TMEM 0x800 + first * 8. Returns where it came from.
        uint32_t load_palette(uint32_t palette_record) {
            uint8_t first = rdp.byte(palette_record + 0x10), last = rdp.byte(palette_record + 0x11);
            uint32_t data = be32(file, palette_record + 0x14) & 0xFFFFFF;
            rdp.image = { data, G_IM_FMT_RGBA, G_IM_SIZ_16b, 1 };
            rdp.tiles[7] = {};
            rdp.tiles[7].tmem = uint16_t((first + 0x100) & 0x1FF);
            rdp.load_tlut(7, 0, 0, uint16_t(((last - first) & 0x3FF) << 2), 0);
            return data;
        }

        // The texture a texture-load list loads, run on its own (state reset), then the palette record given (or
        // no_address): the render tile 0 (the models drawing these leave G_TEXTURE at its default), then any mipmap
        // levels the list sets up in tiles 1-6 (RIM01-21: 32x32 down to 2x2, drawn with texture LOD). Returns tile 0's
        // texture, or -1.
        int list_texture(uint32_t list, uint32_t palette_record) {
            reset();
            current_model.clear();
            run(list);
            uint32_t palette = palette_record != no_address ? load_palette(palette_record) : no_address;
            tlut = othermode_h & (3u << G_MDSFT_TEXTLUT);
            int index = -1;
            for (int i = 0; i < 7; i++) {
                const RT64::LoadTile& t = rdp.tiles[i];
                uint16_t sw, sh;
                sample_size(t, sw, sh);
                if (t.line == 0 || sw > 1024 || sh > 1024 || RT64::TMEMHasher::requiresRawTMEM(t, sw, sh, tlut)) {
                    if (i == 0) return -1;
                    continue;
                }
                uint32_t from = palette;
                if (tlut != 0 && from == no_address) {
                    uint32_t word = (0x100 + (t.siz == G_IM_SIZ_4b ? (t.palette << 4) : 0)) & 0x1FF;
                    from = rdp.loaded_from[word] != no_address ? rdp.loaded_from[word] : rdp.loaded_from[0x100];
                }
                int level = add_texture(t, sw, sh, rdp.loaded_from[t.tmem & 0x1FF], from, true);
                if (i == 0) {
                    index = level;
                }
                else if (level >= 0 && index >= 0 && level != index && mip_of[level] < 0 && out.textures[level].name.empty()) {
                    mip_of[level] = index;
                    mip_level[level] = i;
                }
            }
            return index;
        }

        // A texture table record (func_8007825C, the texture-load builder). A list record (flag 0x08000000 clear) is
        // G_DL'd: its list is run here, so the texture comes out with the game's exact TMEM and hash. A texel record
        // keeps siz at +0x14 and fmt at +0x15, flags 0x10000 / 0x20000 clamp S / T and 0x40000 / 0x80000 mirror them;
        // it's loaded here the way a plain texture would be (TMEM 0, sampled whole). That matches what the builder
        // loads: every texel record a session drew had this hash (17 of 17 in Rush 2), so it gets one, and the menu
        // frames and labels install without being drawn first. Either way the palette record named at +0x16 is loaded
        // after it.
        int record_texture(const std::string& name, uint32_t record, const std::vector<uint32_t>& palettes) {
            uint16_t w = be16(file, record + 0x10), h = be16(file, record + 0x12);
            uint32_t flags = be32(file, record + 0x1C);
            int16_t pal = int16_t(be16(file, record + 0x16));
            uint32_t data = be32(file, record + 0x18) & 0xFFFFFF;
            bool has_palette = pal >= 0 && size_t(pal) < palettes.size();
            if (w == 0 || h == 0 || w > 1024 || h > 1024 || data >= file.size()) {
                return -1;
            }
            reset();
            current_model.clear();
            int index;
            if (!(flags & texel_record)) {
                index = list_texture(data, has_palette ? palettes[pal] : no_address);
                if (index < 0) return -1;
            }
            else {
                uint8_t siz = file[record + 0x14], fmt = file[record + 0x15];
                if (siz > 3) {
                    return -1;
                }
                uint32_t row_bytes = std::max<uint32_t>((uint32_t(w) << siz) >> 1, 1);
                uint32_t line = (row_bytes + 7) / 8;
                if (fmt == G_IM_FMT_RGBA && siz == G_IM_SIZ_32b) {
                    line = (uint32_t(w) * 2 + 7) / 8;
                }
                const uint32_t limit = fmt == G_IM_FMT_CI || siz == G_IM_SIZ_32b ? 2048u : 4096u;
                if (line * h * 8 > limit) {
                    // Too big for one load: the game draws it in strips (2D menu art: MIDWAY, TITLELEFT...). It's
                    // decoded whole here, a band of rows at a time; RT64 hashes the strips, so it has no hash of its
                    // own (strips drawn are traced to it, rush2::derive).
                    uint32_t rows = std::max<uint32_t>(1, limit / (line * 8));
                    Texture tex;
                    tex.texels = data;
                    tex.width = w;
                    tex.height = h;
                    tex.fmt = fmt;
                    tex.siz = siz;
                    tex.tlut = fmt == G_IM_FMT_CI ? G_TT_RGBA16 : 0;
                    tex.image.width = w;
                    for (uint32_t y0 = 0; y0 < h; y0 += rows) {
                        uint32_t n = std::min(rows, h - y0);
                        reset();
                        for (uint32_t y = 0; y < n; y++) {
                            for (uint32_t x = 0; x < row_bytes; x++) {
                                uint32_t a = y * line * 8 + x;
                                if (y & 1) a ^= 4;
                                rdp.tmem[a & tmem_mask8] = rdp.byte(data + (y0 + y) * row_bytes + x);
                            }
                        }
                        if (has_palette) load_palette(palettes[pal]);
                        std::vector<uint8_t> band_indices;
                        Image band = rush2::upscale::decode_tmem(rdp.tmem, fmt, siz, 0, line, 0, tex.tlut, w, n,
                                                                 tex.tlut != 0 ? &band_indices : nullptr);
                        tex.image.rgba.insert(tex.image.rgba.end(), band.rgba.begin(), band.rgba.end());
                        tex.indices.insert(tex.indices.end(), band_indices.begin(), band_indices.end());
                        tex.image.height += n;
                    }
                    tex.key = rush2::upscale::content_key(tex.image);
                    if (auto it = by_key.find(tex.key); it != by_key.end()) {
                        index = it->second;
                    }
                    else {
                        if (tex.tlut != 0) tex.index_key = rush2::upscale::index_key(tex.indices, w, h, siz);
                        tex.edge_s = tex.edge_t = Edge::Clamp;
                        index = int(out.textures.size());
                        out.textures.push_back(std::move(tex));
                        mip_of.push_back(-1);
                        mip_level.push_back(0);
                        palette_from.push_back(has_palette ? palettes[pal] : no_address);
                        by_key.emplace(out.textures[index].key, index);
                    }
                    if (out.textures[index].name.empty()) out.textures[index].name = name;
                    return index;
                }
                // The texels as rows of line words (odd rows have their words swapped, as loads leave them).
                for (uint32_t y = 0; y < h; y++) {
                    for (uint32_t x = 0; x < row_bytes; x++) {
                        uint32_t a = y * line * 8 + x;
                        if (y & 1) a ^= 4;
                        rdp.tmem[a & tmem_mask8] = rdp.byte(data + y * row_bytes + x);
                    }
                }
                uint32_t palette = has_palette ? load_palette(palettes[pal]) : no_address;
                tlut = fmt == G_IM_FMT_CI ? G_TT_RGBA16 : 0;
                RT64::LoadTile t = {};
                t.fmt = fmt; t.siz = siz; t.line = uint16_t(line); t.tmem = 0;
                t.lrs = uint16_t((w - 1) << 2); t.lrt = uint16_t((h - 1) << 2);
                auto log2 = [](uint32_t v) { uint8_t n = 0; while ((1u << n) < v) n++; return n; };
                t.cms = uint8_t(((flags & 0x10000) ? G_TX_CLAMP : 0) | ((flags & 0x40000) ? G_TX_MIRROR : 0));
                t.cmt = uint8_t(((flags & 0x20000) ? G_TX_CLAMP : 0) | ((flags & 0x80000) ? G_TX_MIRROR : 0));
                t.masks = (flags & 0x10000) ? 0 : log2(w);
                t.maskt = (flags & 0x20000) ? 0 : log2(h);
                index = add_texture(t, w, h, data, palette, true);
            }
            if (index >= 0 && out.textures[index].name.empty()) {
                out.textures[index].name = name;
            }
            return index;
        }

        int add_texture(const RT64::LoadTile& t, uint16_t width, uint16_t height, uint32_t texels, uint32_t palette,
                        bool hashable) {
            // A palette the lists never load is the game's (car paint): the hash would hold whatever palette the
            // game loaded, so there is none, and the image shows the shading with a gray ramp.
            const bool painted = tlut != 0 && palette == no_address;
            uint64_t hash = hashable && !painted ? RT64::TMEMHasher::hash(rdp.tmem, t, width, height, tlut, 5) : 0;
            if (hash != 0) {
                auto it = by_hash.find(hash);
                if (it != by_hash.end()) {
                    return it->second;
                }
            }
            std::vector<uint8_t> indices;
            const uint8_t* tmem = rdp.tmem;
            uint8_t gray[tmem_bytes];
            if (painted && paint_record != no_address) {
                // SF Rush's cars carry their paint palettes as records (BMWREDD0...): show the first paint.
                // Loaded into a copy: the draw state (TMEM, and where its palette came from) stays as the lists left it.
                auto image = rdp.image;
                auto tile = rdp.tiles[7];
                uint32_t loaded_from[std::size(rdp.loaded_from)];
                memcpy(loaded_from, rdp.loaded_from, sizeof(loaded_from));
                uint8_t drawn[tmem_bytes];
                memcpy(drawn, rdp.tmem, tmem_bytes);
                load_palette(paint_record);
                memcpy(gray, rdp.tmem, tmem_bytes);
                // SF Rush draws its cars opaque: index 0 (the glass blue) has alpha 0 but shows (tools/rush1/carview.py).
                for (uint32_t a = 0x800; a < tmem_bytes; a += 2) gray[a + 1] |= 1;
                memcpy(rdp.tmem, drawn, tmem_bytes);
                rdp.image = image;
                rdp.tiles[7] = tile;
                memcpy(rdp.loaded_from, loaded_from, sizeof(loaded_from));
                tmem = gray;
            }
            else if (painted) {
                memcpy(gray, rdp.tmem, tmem_bytes);
                gray_palette(gray, t.siz == G_IM_SIZ_4b ? 16 : 256, t.siz == G_IM_SIZ_4b ? t.palette : 0);
                tmem = gray;
            }
            Image image = rush2::upscale::decode_tmem(tmem, t.fmt, t.siz, t.tmem, t.line, t.palette, tlut, width,
                                                      height, tlut != 0 ? &indices : nullptr);
            uint64_t key = rush2::upscale::content_key(image);
            if (hash == 0) {
                auto it = by_key.find(key);
                if (it != by_key.end()) {
                    return it->second;
                }
            }
            Texture tex;
            tex.texels = texels;
            tex.width = width;
            tex.height = height;
            tex.fmt = t.fmt;
            tex.siz = t.siz;
            tex.tlut = tlut;
            tex.edge_s = edge_of(t.cms, t.masks);
            tex.edge_t = edge_of(t.cmt, t.maskt);
            tex.hash = hash;
            tex.key = key;
            if (tlut != 0) {
                tex.index_key = rush2::upscale::index_key(indices, width, height, t.siz);
                tex.painted = painted;
                tex.indices = std::move(indices);
            }
            tex.image = std::move(image);
            int index = int(out.textures.size());
            out.textures.push_back(std::move(tex));
            mip_of.push_back(-1);
            mip_level.push_back(0);
            palette_from.push_back(palette);
            if (hash != 0) by_hash[hash] = index;
            by_key.emplace(key, index);
            return index;
        }

    private:
        const std::vector<uint8_t>& file;
        Container& out;
        bool geometry;
        Rdp rdp;
        std::unordered_map<uint64_t, int> by_hash;
        std::unordered_map<uint64_t, int> by_key; // Textures without a hash.
        std::string current_model;
        Lod* current_lod = nullptr;

        uint32_t tlut = G_TT_RGBA16;     // The draw's TEXTLUT: Rush 2 sets RGBA16 once a frame (geometry.md 5.3).
        uint32_t othermode_h = G_TT_RGBA16;
        uint64_t combine = 0;
        uint8_t tex_tile = 0, tex_levels = 0;
        bool tex_on = true;
        float tex_scale_s = 1, tex_scale_t = 1;
        bool texture_dirty = true;
        std::vector<DrawnTexture> drawn;  // Valid while !texture_dirty: the render tile (and its mipmap levels).

        struct Vtx {
            float x, y, z;
            int16_t s, t;
            uint8_t c[4];
        };
        Vtx vtx[64] = {};

        void reset() {
            rdp.~Rdp();
            new (&rdp) Rdp(file);
            othermode_h = G_TT_RGBA16;
            combine = 0;
            tlut = G_TT_RGBA16;
            tex_on = true; // The model draw leaves texturing on (tile 0, full scale); car lists never send G_TEXTURE.
            tex_tile = 0;
            tex_levels = 0;
            tex_scale_s = tex_scale_t = 1;
            texture_dirty = true;
        }

        void update_textures() {
            if (!texture_dirty) {
                return;
            }
            texture_dirty = false;
            drawn.clear();
            if (!tex_on) {
                return;
            }
            tlut = othermode_h & (3u << G_MDSFT_TEXTLUT);
            bool lod = (othermode_h & (1u << G_MDSFT_TEXTLOD)) != 0;
            bool two_cycle = ((othermode_h >> G_MDSFT_CYCLETYPE) & 3) == 1;
            int count = lod ? tex_levels + 1 : (two_cycle && uses_texel1(combine) ? 2 : 1);
            int base = -1;
            for (int i = 0; i < count; i++) {
                const RT64::LoadTile& t = rdp.tiles[(tex_tile + i) & 7];
                DrawnTexture d = { -1, 0, 0, 0, 0, 1, 1 };
                if (t.line == 0) {
                    drawn.push_back(d);
                    continue;
                }
                uint16_t w, h;
                sample_size(t, w, h);
                if (w > 1024 || h > 1024 || RT64::TMEMHasher::requiresRawTMEM(t, w, h, tlut)) {
                    drawn.push_back(d);
                    continue; // RT64 draws it from raw TMEM, which packs and the observer don't see.
                }
                uint32_t palette = no_address;
                if (tlut != 0) {
                    uint32_t word = (0x100 + (t.siz == G_IM_SIZ_4b ? (t.palette << 4) : 0)) & 0x1FF;
                    palette = rdp.loaded_from[word];
                    if (palette == no_address) palette = rdp.loaded_from[0x100];
                }
                d.index = add_texture(t, w, h, rdp.loaded_from[t.tmem & 0x1FF], palette, true);
                if (i == 0) {
                    base = d.index;
                }
                else if (d.index >= 0 && base >= 0 && d.index != base && mip_of[d.index] < 0 && mip_level[d.index] == 0 &&
                         out.textures[d.index].models.empty()) {
                    mip_of[d.index] = base;
                    mip_level[d.index] = i;
                }
                d.scale_s = (i == 0 ? tex_scale_s : tex_scale_s) * shift_scale(t.shifts) / 32.0f;
                d.scale_t = tex_scale_t * shift_scale(t.shiftt) / 32.0f;
                d.offset_s = t.uls / 4.0f;
                d.offset_t = t.ult / 4.0f;
                d.width = w;
                d.height = h;
                drawn.push_back(d);
            }
            for (const DrawnTexture& d : drawn) {
                if (d.index >= 0) {
                    auto& models = out.textures[d.index].models;
                    if (models.empty() || models.back() != current_model) {
                        if (std::find(models.begin(), models.end(), current_model) == models.end()) {
                            models.push_back(current_model);
                        }
                    }
                }
            }
        }

        void triangle(int a, int b, int c) {
            update_textures();
            if (!geometry || current_lod == nullptr) {
                return;
            }
            int texture = drawn.empty() ? -1 : drawn[0].index;
            auto& meshes = current_lod->meshes;
            if (meshes.empty() || meshes.back().texture != texture) {
                meshes.push_back({});
                meshes.back().texture = texture;
                meshes.back().lit = (current_lod->flags & 0x10) != 0;
                meshes.back().combine = combine;
            }
            Mesh& mesh = meshes.back();
            for (int i : { a, b, c }) {
                const Vtx& v = vtx[i & 63];
                Vertex o;
                o.position[0] = v.x;
                o.position[1] = v.y;
                o.position[2] = v.z;
                if (texture >= 0) {
                    const DrawnTexture& d = drawn[0];
                    o.uv[0] = (v.s * d.scale_s - d.offset_s) / d.width;
                    o.uv[1] = (v.t * d.scale_t - d.offset_t) / d.height;
                }
                else {
                    o.uv[0] = o.uv[1] = 0;
                }
                memcpy(o.color, v.c, 4);
                mesh.vertices.push_back(o);
            }
        }

        void run(uint32_t start) {
            struct Frame { uint32_t pc; };
            std::vector<uint32_t> stack;
            uint32_t pc = start & 0xFFFFFF;
            // Real lists run far fewer commands; a runaway one (data read as a list) stops here instead of decoding
            // textures for minutes.
            size_t budget = 100000;
            while (budget-- > 0) {
                if (pc + 8 > file.size()) {
                    return;
                }
                uint32_t w0 = be32(file, pc), w1 = be32(file, pc + 4);
                pc += 8;
                uint8_t op = w0 >> 24;
                switch (op) {
                    case 0x01: { // G_VTX
                        uint32_t n = (w0 >> 12) & 0xFF;
                        uint32_t end = (w0 >> 1) & 0x7F;
                        uint32_t addr = w1 & 0xFFFFFF;
                        for (uint32_t i = 0; i < n; i++) {
                            uint32_t slot = (end - n + i) & 63;
                            uint32_t a = addr + i * 16;
                            Vtx& v = vtx[slot];
                            v.x = int16_t(be16(file, a));
                            v.y = int16_t(be16(file, a + 2));
                            v.z = int16_t(be16(file, a + 4));
                            v.s = int16_t(be16(file, a + 8));
                            v.t = int16_t(be16(file, a + 10));
                            for (int k = 0; k < 4; k++) v.c[k] = rdp.byte(a + 12 + k);
                        }
                        break;
                    }
                    case 0x05: // G_TRI1
                        triangle(((w0 >> 16) & 0xFF) / 2, ((w0 >> 8) & 0xFF) / 2, (w0 & 0xFF) / 2);
                        break;
                    case 0x06: // G_TRI2
                    case 0x07: // G_QUAD, encoded as two triangles
                        triangle(((w0 >> 16) & 0xFF) / 2, ((w0 >> 8) & 0xFF) / 2, (w0 & 0xFF) / 2);
                        triangle(((w1 >> 16) & 0xFF) / 2, ((w1 >> 8) & 0xFF) / 2, (w1 & 0xFF) / 2);
                        break;
                    case 0xD7: { // G_TEXTURE
                        tex_on = ((w0 >> 1) & 0x7F) != 0;
                        tex_tile = (w0 >> 8) & 7;
                        tex_levels = (w0 >> 11) & 7;
                        tex_scale_s = ((w1 >> 16) & 0xFFFF) / 65536.0f;
                        tex_scale_t = (w1 & 0xFFFF) / 65536.0f;
                        texture_dirty = true;
                        break;
                    }
                    case 0xE3: { // G_SETOTHERMODE_H
                        uint32_t len = (w0 & 0xFF) + 1;
                        uint32_t shift = 32 - ((w0 >> 8) & 0xFF) - len;
                        uint32_t mask = (len >= 32 ? 0xFFFFFFFFu : ((1u << len) - 1)) << shift;
                        othermode_h = (othermode_h & ~mask) | (w1 & mask);
                        texture_dirty = true;
                        break;
                    }
                    case 0xFC: // G_SETCOMBINE
                        combine = (uint64_t(w0 & 0xFFFFFF) << 32) | w1;
                        texture_dirty = true;
                        break;
                    case 0xEF: // G_RDPSETOTHERMODE
                        othermode_h = w0 & 0xFFFFFF;
                        texture_dirty = true;
                        break;
                    case 0xE0: // Conditional op (geometry.md 4.1): skips to w1 unless its load flag holds.
                        if (((w0 >> 16) & 0xFF) == 1 && !((load_flags >> (w0 & 0x1F)) & 1)) {
                            pc = w1 & 0xFFFFFF;
                        }
                        break;
                    case 0xDE: { // G_DL
                        uint32_t target = w1 & 0xFFFFFF;
                        if (target >= file.size()) {
                            break;
                        }
                        if (((w0 >> 16) & 0xFF) == 0) {
                            if (stack.size() >= 10) break;
                            stack.push_back(pc);
                        }
                        pc = target;
                        break;
                    }
                    case 0xDF: // G_ENDDL
                        if (stack.empty()) {
                            return;
                        }
                        pc = stack.back();
                        stack.pop_back();
                        break;
                    case 0xFD: // G_SETTIMG
                        rdp.image.fmt = (w0 >> 21) & 7;
                        rdp.image.siz = (w0 >> 19) & 3;
                        rdp.image.width = uint16_t((w0 & 0xFFF) + 1);
                        rdp.image.address = w1 & 0xFFFFFF;
                        if (auto r = redirect.find(rdp.image.address); r != redirect.end()) rdp.image.address = r->second;
                        break;
                    case 0xF5: { // G_SETTILE
                        auto& t = rdp.tiles[(w1 >> 24) & 7];
                        t.fmt = (w0 >> 21) & 7;
                        t.siz = (w0 >> 19) & 3;
                        t.line = (w0 >> 9) & 0x1FF;
                        t.tmem = w0 & 0x1FF;
                        t.palette = (w1 >> 20) & 0xF;
                        t.cmt = (w1 >> 18) & 3;
                        t.maskt = (w1 >> 14) & 0xF;
                        t.shiftt = (w1 >> 10) & 0xF;
                        t.cms = (w1 >> 8) & 3;
                        t.masks = (w1 >> 4) & 0xF;
                        t.shifts = w1 & 0xF;
                        texture_dirty = true;
                        break;
                    }
                    case 0xF2: { // G_SETTILESIZE
                        auto& t = rdp.tiles[(w1 >> 24) & 7];
                        t.uls = (w0 >> 12) & 0xFFF;
                        t.ult = w0 & 0xFFF;
                        t.lrs = (w1 >> 12) & 0xFFF;
                        t.lrt = w1 & 0xFFF;
                        texture_dirty = true;
                        break;
                    }
                    case 0xF4: // G_LOADTILE
                        rdp.load_tile((w1 >> 24) & 7, (w0 >> 12) & 0xFFF, w0 & 0xFFF, (w1 >> 12) & 0xFFF, w1 & 0xFFF);
                        texture_dirty = true;
                        break;
                    case 0xF3: // G_LOADBLOCK
                        rdp.load_block((w1 >> 24) & 7, (w0 >> 12) & 0xFFF, w0 & 0xFFF, (w1 >> 12) & 0xFFF, w1 & 0xFFF);
                        texture_dirty = true;
                        break;
                    case 0xF0: // G_LOADTLUT
                        rdp.load_tlut((w1 >> 24) & 7, (w0 >> 12) & 0xFFF, w0 & 0xFFF, (w1 >> 12) & 0xFFF, w1 & 0xFFF);
                        texture_dirty = true;
                        break;
                    default:
                        break;
                }
            }
        }
    };

    // The texel (not palette) addresses a texture-load list's SETTIMGs give.
    std::vector<uint32_t> list_images(const std::vector<uint8_t>& file, uint32_t at) {
        std::vector<uint32_t> images;
        for (int i = 0; i < 64 && at + 8 <= file.size(); i++, at += 8) {
            uint32_t w0 = be32(file, at), w1 = be32(file, at + 4);
            if ((w0 >> 24) == 0xDF) break;
            if ((w0 >> 24) == 0xFD) images.push_back(w1 & 0xFFFFFF);
        }
        return images;
    }
}

std::string file_name(const std::string& name) {
    std::string s;
    for (char c : name) {
        bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '.' || c == '_' ||
                  c == '-' || c == '~';
        s += ok ? c : '_';
    }
    if (s.empty() || s == "." || s == "..") {
        s = "_" + s;
    }
    return s;
}

bool read_container(const std::vector<uint8_t>& file, Container& out, std::string& error, bool geometry,
                    bool paint_records, const std::map<uint32_t, uint32_t>* copies) {
    out = {};
    if (file.size() < 0x28) {
        error = "too small";
        return false;
    }
    uint32_t models_at = be32(file, 0), names_at = be32(file, 4), textures_at = be32(file, 8), palettes_at = be32(file, 12);
    uint32_t model_count = be32(file, 16), texture_count = be32(file, 20), palette_count = be32(file, 24);
    // Texture banks (rims, car select art...) are containers with no models.
    if ((model_count == 0 && texture_count == 0) || model_count > 4096 || texture_count > 4096 || palette_count > 4096 ||
        size_t(models_at) + model_count * 0x34 > file.size() || size_t(names_at) + model_count * 0x18 > file.size() ||
        size_t(textures_at) + texture_count * 0x20 > file.size() || size_t(palettes_at) + palette_count * 0x18 > file.size()) {
        error = "not a model container";
        return false;
    }

    Walker walker(file, out, geometry);
    std::vector<uint32_t> palettes;
    for (uint32_t i = 0; i < palette_count; i++) {
        palettes.push_back(palettes_at + i * 0x18); // The records: load_palette reads their range and data.
        // Paint palettes in the file (SF Rush's cars; Rush 2's have none): a painted texture shows the red one, the
        // paint SF Rush starts on.
        std::string name = cstr(file, palettes.back(), 16);
        if (!paint_records) continue;
        if (walker.paint_record == no_address || (name.find("RED") != std::string::npos &&
                                                  cstr(file, walker.paint_record, 16).find("RED") == std::string::npos)) {
            walker.paint_record = palettes.back();
        }
    }
    for (uint32_t i = 0; i < texture_count; i++) {
        uint32_t r = textures_at + i * 0x20;
        std::string name = cstr(file, r, 16);
        uint32_t data = be32(file, r + 0x18) & 0xFFFFFF;
        if (!(be32(file, r + 0x1C) & texel_record)) {
            std::vector<uint32_t> images = list_images(file, data);
            // A load list loads its palette first when it has one; its texels are its last SETTIMG.
            if (!images.empty()) walker.record_names.emplace(images.back(), name);
        }
        else {
            walker.record_names.emplace(data, name);
        }
    }

    for (uint32_t m = 0; m < model_count; m++) {
        uint32_t r = models_at + m * 0x34, n = names_at + m * 0x18;
        Model model;
        model.name = cstr(file, n, 16);
        model.radius = bef(file, n + 0x10);
        model.kind = be16(file, n + 0x14);
        uint32_t lods = std::min<uint32_t>(be32(file, r), 4);
        for (uint32_t l = 0; l < lods; l++) {
            uint32_t e = r + 4 + l * 12;
            Lod lod;
            lod.flags = be16(file, e + 2);
            lod.distance = bef(file, e + 4);
            model.lods.push_back(std::move(lod));
        }
        out.models.push_back(std::move(model));
    }
    for (uint32_t m = 0; m < model_count; m++) {
        uint32_t r = models_at + m * 0x34;
        Model& model = out.models[m];
        for (uint32_t l = 0; l < model.lods.size(); l++) {
            uint32_t list = be32(file, r + 4 + l * 12 + 8) & 0xFFFFFF;
            if (list != 0 && list < file.size()) {
                walker.run_lod(model.name, list, &model.lods[l]);
            }
        }
    }
    // The texture table: textures code loads by name through the builder (car parts' swaps, checkpoint banners...).
    // Ones whose texels the lists drew already just give those their name.
    for (uint32_t i = 0; i < texture_count; i++) {
        uint32_t r = textures_at + i * 0x20;
        uint32_t data = be32(file, r + 0x18) & 0xFFFFFF;
        bool drawn = false;
        if (be32(file, r + 0x1C) & texel_record) {
            for (const Texture& t : out.textures) {
                drawn |= t.texels == data;
            }
        }
        if (!drawn) {
            walker.record_texture(cstr(file, r, 16), r, palettes);
        }
    }

    // Copies of textures the code points the lists at instead (a 2049 car's scuffed panels): the models walked again
    // with the copies' texels, each named after its texture with _D1, as Rush 2's own damaged panels are.
    if (copies != nullptr && !copies->empty()) {
        walker.redirect = *copies;
        for (uint32_t m = 0; m < model_count; m++) {
            uint32_t r = models_at + m * 0x34;
            for (uint32_t l = 0; l < out.models[m].lods.size(); l++) {
                uint32_t list = be32(file, r + 4 + l * 12 + 8) & 0xFFFFFF;
                if (list != 0 && list < file.size()) walker.run_lod(out.models[m].name, list, nullptr);
            }
        }
        walker.redirect.clear();
        for (const auto& [from, to] : *copies) {
            if (auto it = walker.record_names.find(from); it != walker.record_names.end()) {
                walker.record_names[to] = it->second + "_D1";
            }
        }
    }

    // Every texture-load list of the file (header +28 .. +32), run on its own: a texture only code points a model at
    // (the Rocket ZX's flame frames, which its animation switches between) is the file's as much as the drawn ones.
    // Named LOADLIST.n unless a record names its texels.
    uint32_t txld_start = be32(file, 28), txld_end = be32(file, 32);
    if (txld_start < txld_end && txld_end <= file.size() && txld_start % 8 == 0) {
        std::set<uint32_t> have;
        for (const Texture& t : out.textures) have.insert(t.texels);
        for (uint32_t o = txld_start; o < txld_end;) {
            uint32_t list = o;
            while (o + 8 <= txld_end && file[o] != 0xDF) o += 8;
            o += 8;
            std::vector<uint32_t> images = list_images(file, list);
            if (images.empty() || have.count(images.back())) continue;
            int index = walker.list_texture(list, no_address);
            if (index >= 0) {
                have.insert(out.textures[index].texels);
                if (out.textures[index].models.empty()) out.textures[index].models.push_back("LOADLIST");
            }
        }
    }

    // Names: the record's for its texels, else the first model drawing it; then "~n" for other draws of the same
    // texels and ".mipN" for mipmap levels.
    std::map<uint32_t, std::string> base_names = walker.record_names;
    std::map<std::string, int> per_model;   // Textures each model introduces, to number them.
    for (size_t i = 0; i < out.textures.size(); i++) {
        const Texture& t = out.textures[i];
        if (!t.name.empty() || walker.mip_of[i] >= 0 || base_names.count(t.texels)) continue;
        std::string model = t.models.empty() ? "UNUSED" : t.models[0];
        per_model[model]++;
    }
    std::map<std::string, int> numbered;
    std::set<std::string> used;
    std::map<uint32_t, int> variants;
    auto unique = [&](std::string name) {
        std::string candidate = name;
        for (int n = 2; used.count(candidate); n++) {
            candidate = name + "~" + std::to_string(n);
        }
        used.insert(candidate);
        return candidate;
    };
    for (size_t i = 0; i < out.textures.size(); i++) {
        Texture& t = out.textures[i];
        if (walker.mip_of[i] >= 0) continue;
        std::string base;
        if (!t.name.empty()) {
            base = t.name;
        }
        else if (auto it = base_names.find(t.texels); it != base_names.end()) {
            base = it->second;
        }
        else {
            std::string model = t.models.empty() ? "UNUSED" : t.models[0];
            if (per_model[model] > 1) {
                base = model + "." + std::to_string(++numbered[model]);
            }
            else {
                base = model;
            }
            if (t.texels != no_address) {
                base_names[t.texels] = base; // Later draws of these texels are its variants.
            }
        }
        t.name = unique(base);
    }
    for (size_t i = 0; i < out.textures.size(); i++) {
        if (walker.mip_of[i] >= 0) {
            out.textures[i].name = unique(out.textures[walker.mip_of[i]].name + ".mip" + std::to_string(walker.mip_level[i]));
        }
    }
    return true;
}

std::vector<Placement> read_placement(const std::vector<uint8_t>& file) {
    std::vector<Placement> out;
    uint32_t trees = be32(file, 0);
    if (trees == 0 || trees > 64 || file.size() < 4 + trees * 20) {
        return out;
    }
    uint32_t first = be32(file, 4);
    for (uint32_t i = 1; i < trees; i++) {
        first = std::min(first, be32(file, 4 + i * 20));
    }
    for (size_t o = first; o + 0x64 <= file.size(); o += 0x64) {
        Placement p;
        p.name = cstr(file, o, 16);
        for (int k = 0; k < 9; k++) p.matrix[k] = bef(file, o + 0x10 + k * 4);
        for (int k = 0; k < 3; k++) p.position[k] = bef(file, o + 0x34 + k * 4);
        p.flags = be32(file, o + 0x40);
        out.push_back(std::move(p));
    }
    return out;
}

// glTF 2.0 binary (https://registry.khronos.org/glTF/specs/2.0/glTF-2.0.html): a JSON chunk and one BIN chunk.
Container single_model(const Container& container, size_t m) {
    Container out;
    out.models.push_back(container.models[m]);
    std::map<int, int> remap;
    for (Lod& lod : out.models[0].lods) {
        for (Mesh& mesh : lod.meshes) {
            if (mesh.texture < 0) continue;
            auto [it, added] = remap.emplace(mesh.texture, int(out.textures.size()));
            if (added) out.textures.push_back(container.textures[size_t(mesh.texture)]);
            mesh.texture = it->second;
        }
    }
    return out;
}

std::vector<uint8_t> write_glb(const Container& container, const std::string& title,
                               const std::vector<Placement>& placements) {
    std::vector<uint8_t> bin;
    auto align = [&]() { while (bin.size() % 4) bin.push_back(0); };
    auto put = [&](const void* p, size_t n) {
        const uint8_t* b = static_cast<const uint8_t*>(p);
        bin.insert(bin.end(), b, b + n);
    };
    auto json_string = [](const std::string& s) {
        std::string o = "\"";
        for (char c : s) {
            if (c == '"' || c == '\\') o += '\\';
            if (uint8_t(c) < 0x20) continue;
            o += c;
        }
        return o + "\"";
    };
    auto num = [](float f) {
        char buf[32];
        snprintf(buf, sizeof(buf), "%.9g", std::isfinite(f) ? f : 0.0f);
        return std::string(buf);
    };

    std::string views, accessors, meshes, images, textures, materials, samplers;
    int view_count = 0, accessor_count = 0, mesh_count = 0;
    auto add_view = [&](size_t offset, size_t length, int target) {
        if (!views.empty()) views += ",";
        views += "{\"buffer\":0,\"byteOffset\":" + std::to_string(offset) + ",\"byteLength\":" + std::to_string(length);
        if (target) views += ",\"target\":" + std::to_string(target);
        views += "}";
        return view_count++;
    };

    // Textures: one image, sampler and texture each; materials per (texture, lit) pair.
    std::vector<int> texture_index(container.textures.size(), -1);
    int texture_count = 0;
    for (size_t i = 0; i < container.textures.size(); i++) {
        const Texture& t = container.textures[i];
        if (t.image.empty()) continue;
        std::vector<uint8_t> png;
        if (!rush2::upscale::write_png_bytes(t.image, png)) continue;
        align();
        size_t offset = bin.size();
        put(png.data(), png.size());
        int view = add_view(offset, png.size(), 0);
        if (!images.empty()) { images += ","; textures += ","; samplers += ","; }
        images += "{\"name\":" + json_string(t.name) + ",\"mimeType\":\"image/png\",\"bufferView\":" + std::to_string(view) + "}";
        auto wrap = [](Edge e) { return e == Edge::Clamp ? 33071 : e == Edge::Mirror ? 33648 : 10497; };
        samplers += "{\"magFilter\":9729,\"minFilter\":9987,\"wrapS\":" + std::to_string(wrap(t.edge_s)) +
                    ",\"wrapT\":" + std::to_string(wrap(t.edge_t)) + "}";
        textures += "{\"name\":" + json_string(t.name) + ",\"sampler\":" + std::to_string(texture_count) +
                    ",\"source\":" + std::to_string(texture_count) + "}";
        texture_index[i] = texture_count++;
    }
    std::map<std::pair<int, bool>, int> material_index;
    int material_count = 0;
    auto material = [&](int texture, bool lit) {
        auto key = std::make_pair(texture, lit);
        auto it = material_index.find(key);
        if (it != material_index.end()) return it->second;
        const Texture* t = texture >= 0 ? &container.textures[texture] : nullptr;
        std::string alpha = "OPAQUE";
        if (t != nullptr) {
            bool clear = false, graded = false;
            for (size_t p = 3; p < t->image.rgba.size(); p += 4) {
                uint8_t a = t->image.rgba[p];
                clear |= a == 0;
                graded |= a != 0 && a != 255;
            }
            alpha = graded ? "BLEND" : clear ? "MASK" : "OPAQUE";
        }
        if (!materials.empty()) materials += ",";
        materials += "{\"name\":" + json_string(t != nullptr ? t->name : std::string("untextured")) +
                     ",\"pbrMetallicRoughness\":{\"metallicFactor\":0,\"roughnessFactor\":1";
        if (t != nullptr && texture_index[texture] >= 0) {
            materials += ",\"baseColorTexture\":{\"index\":" + std::to_string(texture_index[texture]) + "}";
        }
        materials += "},\"alphaMode\":\"" + alpha + "\",\"doubleSided\":false";
        if (!lit) materials += ",\"extensions\":{\"KHR_materials_unlit\":{}}";
        materials += "}";
        return material_index[key] = material_count++;
    };

    // One node per LOD; the first LOD's node is the model's, with the others as its children (scale 0, so viewers
    // show the model once).
    std::vector<std::string> node_list;
    std::string scene_nodes;
    std::vector<int> model_mesh(container.models.size(), -1); // Mesh of each model's first LOD.
    for (const Model& model : container.models) {
        int first = -1;
        std::vector<int> others;
        for (size_t l = 0; l < model.lods.size(); l++) {
            const Lod& lod = model.lods[l];
            std::string primitives;
            for (const Mesh& m : lod.meshes) {
                if (m.vertices.empty()) continue;
                size_t n = m.vertices.size();
                float lo[3] = { 1e30f, 1e30f, 1e30f }, hi[3] = { -1e30f, -1e30f, -1e30f };
                align();
                size_t pos_at = bin.size();
                for (const Vertex& v : m.vertices) {
                    // glTF is right-handed; the game's model space is the mirror image of it (x to the left), so x is
                    // negated: the models face the right way round (text reads) and their triangles, which the game
                    // winds clockwise, come out counter-clockwise, glTF's front faces.
                    const float p[3] = { -v.position[0], v.position[1], v.position[2] };
                    put(p, 12);
                    for (int k = 0; k < 3; k++) { lo[k] = std::min(lo[k], p[k]); hi[k] = std::max(hi[k], p[k]); }
                }
                size_t uv_at = bin.size();
                for (const Vertex& v : m.vertices) put(v.uv, 8);
                size_t color_at = bin.size();
                for (const Vertex& v : m.vertices) {
                    uint8_t c[4] = { v.color[0], v.color[1], v.color[2], v.color[3] };
                    if (m.lit) { c[0] = c[1] = c[2] = 255; } // Normals, not colors.
                    put(c, 4);
                }
                int pos_view = add_view(pos_at, n * 12, 34962);
                int uv_view = add_view(uv_at, n * 8, 34962);
                int color_view = add_view(color_at, n * 4, 34962);
                auto accessor = [&](int view, const char* type, int component, bool normalized, const std::string& extra) {
                    if (!accessors.empty()) accessors += ",";
                    accessors += "{\"bufferView\":" + std::to_string(view) + ",\"componentType\":" +
                                 std::to_string(component) + ",\"count\":" + std::to_string(n) + ",\"type\":\"" + type + "\"";
                    if (normalized) accessors += ",\"normalized\":true";
                    accessors += extra + "}";
                    return accessor_count++;
                };
                int pos_acc = accessor(pos_view, "VEC3", 5126, false,
                    ",\"min\":[" + num(lo[0]) + "," + num(lo[1]) + "," + num(lo[2]) + "],\"max\":[" + num(hi[0]) + "," +
                    num(hi[1]) + "," + num(hi[2]) + "]");
                int uv_acc = accessor(uv_view, "VEC2", 5126, false, "");
                int color_acc = accessor(color_view, "VEC4", 5121, true, "");
                if (!primitives.empty()) primitives += ",";
                primitives += "{\"attributes\":{\"POSITION\":" + std::to_string(pos_acc) + ",\"TEXCOORD_0\":" +
                              std::to_string(uv_acc) + ",\"COLOR_0\":" + std::to_string(color_acc) + "},\"material\":" +
                              std::to_string(material(m.texture, m.lit)) + "}";
            }
            std::string node_name = l == 0 ? model.name : model.name + " LOD " + std::to_string(l);
            std::string node = "{\"name\":" + json_string(node_name);
            if (!primitives.empty()) {
                if (!meshes.empty()) meshes += ",";
                meshes += "{\"name\":" + json_string(node_name) + ",\"primitives\":[" + primitives + "]}";
                if (l == 0) model_mesh[&model - container.models.data()] = mesh_count;
                node += ",\"mesh\":" + std::to_string(mesh_count++);
            }
            node += ",\"extras\":{\"distance\":" + num(lod.distance) + ",\"flags\":" + std::to_string(lod.flags);
            if (l == 0) node += ",\"kind\":" + std::to_string(model.kind) + ",\"radius\":" + num(model.radius);
            node += "}";
            if (l > 0) node += ",\"scale\":[0,0,0]";
            if (l == 0) {
                first = int(node_list.size());
            }
            else {
                others.push_back(int(node_list.size()));
            }
            node_list.push_back(node);
        }
        if (first < 0) continue;
        if (!others.empty()) {
            std::string children;
            for (int o : others) children += (children.empty() ? "" : ",") + std::to_string(o);
            node_list[first] += ",\"children\":[" + children + "]";
        }
        if (!scene_nodes.empty()) scene_nodes += ",";
        scene_nodes += std::to_string(first);
    }
    // Placed records: the record's model, else the model its class draws (CONE1 -> CONE1O1: the shortest model name
    // starting with the record's). Sound emitters, keys and collision volumes have no model here and are left out.
    std::string placed_nodes;
    if (!placements.empty()) {
        std::map<std::string, int> by_name;
        for (size_t m = 0; m < container.models.size(); m++) by_name.emplace(container.models[m].name, int(m));
        for (const Placement& p : placements) {
            int m = -1;
            if (auto it = by_name.find(p.name); it != by_name.end()) {
                m = it->second;
            }
            else if (!p.name.empty()) {
                for (auto it2 = by_name.lower_bound(p.name); it2 != by_name.end() && it2->first.rfind(p.name, 0) == 0; ++it2) {
                    if (m < 0 || it2->first.size() < container.models[m].name.size()) m = it2->second;
                }
            }
            if (m < 0 || model_mesh[m] < 0) continue;
            // glTF matrices are column-major for column vectors; the game's rows multiply row vectors, so the rows go
            // in as columns. Models are drawn at 1/16 scale (vertex units / 16 = world units), so the placed scene is
            // in the game's world units. World x is negated like the vertices' (S M S with S = diag(-1, 1, 1): the
            // entries of exactly one x row or column change sign).
            std::string matrix;
            for (int r = 0; r < 3; r++) {
                for (int c = 0; c < 3; c++) {
                    float sign = (r == 0) != (c == 0) ? -1.0f : 1.0f;
                    matrix += num(sign * p.matrix[r * 3 + c] / 16.0f) + ",";
                }
                matrix += "0,";
            }
            matrix += num(-p.position[0]) + "," + num(p.position[1]) + "," + num(p.position[2]) + ",1";
            if (!placed_nodes.empty()) placed_nodes += ",";
            placed_nodes += std::to_string(node_list.size());
            node_list.push_back("{\"name\":" + json_string(p.name) + ",\"mesh\":" + std::to_string(model_mesh[m]) +
                                ",\"matrix\":[" + matrix + "],\"extras\":{\"model\":" +
                                json_string(container.models[m].name) + ",\"flags\":" + std::to_string(p.flags) + "}");
        }
    }
    std::string nodes;
    for (const std::string& node : node_list) {
        nodes += (nodes.empty() ? "" : ",") + node + "}";
    }
    align();

    std::string json = "{\"asset\":{\"version\":\"2.0\",\"generator\":\"Rush 2 Recompiled\"},"
                       "\"extensionsUsed\":[\"KHR_materials_unlit\"],\"scene\":0,\"scenes\":[";
    if (!placed_nodes.empty()) {
        json += "{\"name\":" + json_string(title) + ",\"nodes\":[" + placed_nodes + "]},{\"name\":" +
                json_string(title + " models") + ",\"nodes\":[" + scene_nodes + "]}";
    }
    else {
        json += "{\"name\":" + json_string(title) + ",\"nodes\":[" + scene_nodes + "]}";
    }
    json += "],\"nodes\":[" + nodes + "]";
    if (!meshes.empty()) json += ",\"meshes\":[" + meshes + "]";
    if (!materials.empty()) json += ",\"materials\":[" + materials + "]";
    if (!images.empty()) json += ",\"images\":[" + images + "],\"samplers\":[" + samplers + "],\"textures\":[" + textures + "]";
    if (!accessors.empty()) json += ",\"accessors\":[" + accessors + "]";
    if (!views.empty()) json += ",\"bufferViews\":[" + views + "]";
    json += ",\"buffers\":[{\"byteLength\":" + std::to_string(bin.size()) + "}]}";
    while (json.size() % 4) json += ' ';

    std::vector<uint8_t> glb;
    auto u32 = [&](uint32_t v) { for (int i = 0; i < 4; i++) glb.push_back(uint8_t(v >> (i * 8))); };
    u32(0x46546C67); // "glTF"
    u32(2);
    u32(uint32_t(12 + 8 + json.size() + 8 + bin.size()));
    u32(uint32_t(json.size()));
    u32(0x4E4F534A); // "JSON"
    glb.insert(glb.end(), json.begin(), json.end());
    u32(uint32_t(bin.size()));
    u32(0x004E4942); // "BIN"
    glb.insert(glb.end(), bin.begin(), bin.end());
    return glb;
}

}
