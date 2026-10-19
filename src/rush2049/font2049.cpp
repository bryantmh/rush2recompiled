// Rush 2049's fonts (include/font2049.h).
//
// Atlas (file 22 + n): a model file whose TXHD records (0x24 bytes) are the pages: +16 width, +18 height (u16), +20 the
// texel format (top byte 1: 8 bits a texel, IA8 with the coverage in the low nibble; else I4), +24 the texels' offset
// from the IMAG chunk. Glyph table (file 38 + n): a 20-byte header (+2 the font's size, +5 a line's height, +7 a space's
// width, +8 the gap between characters, +0xB the page count), then 12-byte records: char, page, x0, y0, x1, y1
// (inclusive).

#include <cstring>

#include "font2049.h"

namespace {
    uint32_t be32(const std::vector<uint8_t>& d, size_t o) {
        return (uint32_t)d[o] << 24 | (uint32_t)d[o + 1] << 16 | (uint32_t)d[o + 2] << 8 | d[o + 3];
    }
}

std::optional<rush2::font2049::Font> rush2::font2049::load(const rush2::rom2049::Source& rom, int n) {
    std::vector<uint8_t> atlas, table;
    if (rom.is_dreamcast() || !rom.read_file(22 + n, atlas) || !rom.read_file(38 + n, table) || atlas.size() < 4 ||
        table.size() < 20) {
        return std::nullopt;
    }
    Font font;
    font.line_height = table[5];
    font.space = table[7];
    font.gap = table[8];
    uint32_t imag = 0, txhd = 0, txhd_n = 0;
    for (size_t o = be32(atlas, 0); o + 12 <= atlas.size(); o += 12) {
        if (memcmp(&atlas[o], "IMAG", 4) == 0) imag = be32(atlas, o + 4);
        else if (memcmp(&atlas[o], "TXHD", 4) == 0) { txhd = be32(atlas, o + 4); txhd_n = be32(atlas, o + 8); }
    }
    for (uint32_t i = 0; i < txhd_n; i++) {
        size_t rec = txhd + size_t(i) * 0x24;
        if (rec + 0x24 > atlas.size()) return std::nullopt;
        Font::Page p;
        p.w = (atlas[rec + 16] << 8) | atlas[rec + 17];
        p.h = (atlas[rec + 18] << 8) | atlas[rec + 19];
        bool ia8 = atlas[rec + 20] == 1;
        size_t texels = imag + be32(atlas, rec + 24);
        size_t bytes = ia8 ? size_t(p.w * p.h) : size_t(p.w * p.h + 1) / 2;
        if (p.w <= 0 || p.h <= 0 || texels + bytes > atlas.size()) return std::nullopt;
        p.coverage.resize(size_t(p.w * p.h));
        for (int t = 0; t < p.w * p.h; t++) {
            int v = ia8 ? atlas[texels + t] & 15 : (atlas[texels + t / 2] >> ((t & 1) ? 0 : 4)) & 15;
            p.coverage[t] = uint8_t(v * 17);
        }
        font.pages.push_back(std::move(p));
    }
    for (size_t o = 20; o + 12 <= table.size(); o += 12) {
        Font::Glyph g{ table[o + 1], table[o + 2], table[o + 3], table[o + 4], table[o + 5] };
        if (table[o] == 0 || g.page >= int(font.pages.size()) || g.x1 < g.x0 || g.y1 < g.y0 ||
            g.x1 >= font.pages[g.page].w || g.y1 >= font.pages[g.page].h) continue;
        font.glyphs[char(table[o])] = g;
    }
    if (font.glyphs.count('A') == 0 && font.glyphs.count('0') == 0) return std::nullopt;
    return font;
}
