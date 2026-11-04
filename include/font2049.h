#ifndef __FONT2049_H__
#define __FONT2049_H__

#include <array>
#include <map>
#include <optional>
#include <vector>

#include "rush2049_rom.h"

// Rush 2049's fonts (src/rush2049/font2049.cpp; docs/rush2049_research/menus.md section 10): font n is atlas file
// 22 + n and glyph table file 38 + n of the N64 ROM. Used for the 2049 cars' name logos (font 13, AERO-PLAIN16) and the
// Rush 2049 Stunt HUD (fonts 10, 4BAERO-PLAIN12, and 12, AERO-PLAIN7).
namespace rush2::font2049 {
    struct Font {
        // Atlas pages, rows top down: each texel's coverage 0-255 (I4 pages' intensity, IA8 pages' low nibble, x 17).
        struct Page { int w = 0, h = 0; std::vector<uint8_t> coverage; };
        // A character's cell in its page, inclusive (cells are top-aligned in a line).
        struct Glyph { int page, x0, y0, x1, y1; };
        std::vector<Page> pages;
        std::map<char, Glyph> glyphs;
        // From the glyph table's header: a line's height (byte 5), a space's width (byte 7) and the gap between
        // characters (byte 8; func_800B3E50 copies it to 0x80149B70, which func_800B3FA4 adds after each character).
        int line_height = 0, space = 0, gap = 1;
        // Bytes 2 and 3: the ascent and descent; func_800B71D4 centers a line on y by half their sum (vertical mode 1).
        int ascent = 0, descent = 0;
        // Kerning (header byte 10 set): a row of `count` bytes per glyph record after the records; func_800B3FA4 and
        // the drawer take kern[this glyph's record][the previous glyph's record] off a character's advance, unless the
        // previous glyph is record 0 or a space came between. record[c] is character c's record (-1 none).
        int count = 0;
        bool kerning = false;
        std::vector<uint8_t> kern;
        std::array<int16_t, 256> record{};
    };

    // Font n of the N64 source (std::nullopt for a Dreamcast source or a file that doesn't parse).
    std::optional<Font> load(const rush2::rom2049::Source& rom, int n);
}

#endif
