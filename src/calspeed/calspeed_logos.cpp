// California Speed's cars' car select name logos (docs/calspeed_research.md#car-name-art).
//
// California Speed has no logo for its own cars: its car select bank (asset 9) holds Rush 2's 22 car logos (Pickup,
// Compact, Muscle Car...) redrawn in its own lettering, a bold pixel sans in a blue gradient with a dark edge, all on one
// shared palette. Three of its cars are among those words (Sportster, Muscle Car, Mtn Dew) and use them as they are.
// The others are spelled from letters cut out of the words: each letter is the pixels inside a column window of a word
// that belong to a piece holding fill (so a neighbor's touching edge is left behind). The letters the words never use
// are put together from real ones (I is the l, O and 8 are C and S mirrored onto themselves, 6 is the S's top on the
// 8's bottom loop, G is the C closed by the O's lower right and a bar, q is p mirrored, f is r's arm on an l stem with
// t's crossbar, j is i with p's descender, v is V scaled to the x-height); only z is drawn, colored with the
// lowercase letters' own row gradient. The letters keep their rows (all the words share one baseline) and are set a
// texel apart. The result is cached on disk (track_cache), keyed on the ROM and the build.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "calspeed.h"
#include "car2049.h"
#include "track_cache.h"

namespace {
    using rush2::calspeed::car_count;

    constexpr int logo_asset = 9;
    constexpr int w = 128, h = 32;
    constexpr uint8_t edge = 13; // a dark edge texel (palette 12-15 edge, 1-11 the fill from light to dark)
    constexpr uint32_t cache_version = 1;

    // Car n's logo: "@WORD" is the bank's own logo, anything else is spelled from its letters.
    constexpr const char* logo_texts[car_count] = { "@SPORTSTER", "@MUSCLE", "486SE", "Predator", "Mano", "Five Oh",
        "@DEW", "Hopper", "Fairchild", "Insect", "Mercado", "Golf Cart", "Squirrel", "Forklift", "Conv", "Sled", "Baja",
        "Ol Truck", "Camper", "Semi", "Dozer" };

    bool is_fill(uint8_t v) { return v >= 1 && v <= 11; }
    uint32_t be32(const std::vector<uint8_t>& d, size_t o) {
        return (uint32_t(d[o]) << 24) | (uint32_t(d[o + 1]) << 16) | (uint32_t(d[o + 2]) << 8) | d[o + 3];
    }

    // A word of the bank: palette indices, rows top down.
    struct Word {
        std::vector<uint8_t> px;
        uint32_t palette = 0;
        uint8_t at(int x, int y) const { return px[size_t(y * w + x)]; }
    };

    // Pixels by (x, y): an empty texel is absent.
    using Glyph = std::map<std::pair<int, int>, uint8_t>;

    int width(const Glyph& g) {
        int m = -1;
        for (const auto& [p, v] : g) m = std::max(m, p.first);
        return m + 1;
    }
    Glyph normalize(const Glyph& g) {
        if (g.empty()) return g;
        int m = g.begin()->first.first;
        for (const auto& [p, v] : g) m = std::min(m, p.first);
        Glyph out;
        for (const auto& [p, v] : g) out[{ p.first - m, p.second }] = v;
        return out;
    }
    Glyph hflip(const Glyph& g) {
        int gw = width(g);
        Glyph out;
        for (const auto& [p, v] : g) out[{ gw - 1 - p.first, p.second }] = v;
        return out;
    }
    // Later glyphs fill empty texels, and their fill wins over earlier edges.
    Glyph unite(std::initializer_list<Glyph> gs) {
        Glyph out;
        for (const Glyph& g : gs) {
            for (const auto& [p, v] : g) {
                auto it = out.find(p);
                if (it == out.end() || (is_fill(v) && !is_fill(it->second))) out[p] = v;
            }
        }
        return out;
    }
    Glyph region(const Glyph& g, int x0, int x1, int y0, int y1) {
        Glyph out;
        for (const auto& [p, v] : g) {
            if (p.first >= x0 && p.first <= x1 && p.second >= y0 && p.second <= y1) out[p] = v;
        }
        return out;
    }
    Glyph shift(const Glyph& g, int dx, int dy) {
        Glyph out;
        for (const auto& [p, v] : g) out[{ p.first + dx, p.second + dy }] = v;
        return out;
    }
    // Without the texels at x >= x0 and y >= y0 (a neighbor's piece the window couldn't leave out).
    Glyph drop_corner(const Glyph& g, int x0, int y0) {
        Glyph out;
        for (const auto& [p, v] : g) {
            if (!(p.first >= x0 && p.second >= y0)) out[p] = v;
        }
        return out;
    }

    // The pixels of `word` in columns x0..x1 whose 4-connected piece inside the window holds at least 3 fill texels.
    Glyph cut(const Word& word, int x0, int x1) {
        Glyph keep;
        std::vector<uint8_t> seen(size_t(w * h), 0);
        for (int y = 0; y < h; y++) {
            for (int x = x0; x <= x1; x++) {
                if (word.at(x, y) == 0 || seen[size_t(y * w + x)]) continue;
                std::vector<std::pair<int, int>> piece, stack{ { x, y } };
                seen[size_t(y * w + x)] = 1;
                while (!stack.empty()) {
                    auto [cx, cy] = stack.back();
                    stack.pop_back();
                    piece.push_back({ cx, cy });
                    const int step[4][2] = { { 1, 0 }, { -1, 0 }, { 0, 1 }, { 0, -1 } };
                    for (const auto& s : step) {
                        int nx = cx + s[0], ny = cy + s[1];
                        if (nx < x0 || nx > x1 || ny < 0 || ny >= h || seen[size_t(ny * w + nx)] || word.at(nx, ny) == 0) continue;
                        seen[size_t(ny * w + nx)] = 1;
                        stack.push_back({ nx, ny });
                    }
                }
                int fill = 0;
                for (const auto& [px, py] : piece) fill += is_fill(word.at(px, py));
                if (fill < 3) continue;
                for (const auto& [px, py] : piece) keep[{ px - x0, py }] = word.at(px, py);
            }
        }
        return normalize(keep);
    }

    // The middle (lower of two) fill index of each row over `letters`: the gradient drawn strokes are colored with.
    std::map<int, uint8_t> row_colors(const std::map<char, Glyph>& glyphs, const char* letters) {
        std::map<int, std::vector<uint8_t>> rows;
        for (const char* c = letters; *c; c++) {
            for (const auto& [p, v] : glyphs.at(*c)) {
                if (is_fill(v)) rows[p.second].push_back(v);
            }
        }
        std::map<int, uint8_t> out;
        for (auto& [y, v] : rows) {
            std::sort(v.begin(), v.end());
            out[y] = v[(v.size() - 1) / 2];
        }
        return out;
    }

    // A glyph from a '#' mask whose first row is `top`: fill colored by row, an edge texel right of and below each.
    Glyph from_mask(std::initializer_list<const char*> rows, int top, const std::map<int, uint8_t>& colors) {
        Glyph g;
        int dy = 0;
        for (const char* line : rows) {
            for (int x = 0; line[x]; x++) {
                if (line[x] == '#') g[{ x, top + dy }] = colors.count(top + dy) ? colors.at(top + dy) : 6;
            }
            dy++;
        }
        Glyph out = g;
        for (const auto& [p, v] : g) {
            for (std::pair<int, int> q : { std::pair{ p.first + 1, p.second }, std::pair{ p.first, p.second + 1 } }) {
                if (g.count(q) == 0 && out.count(q) == 0) out[q] = edge;
            }
        }
        return out;
    }

    // g's rows src_top..src_bottom resampled (nearest) into dst_top..dst_bottom, its columns scaled by sx.
    Glyph rescale(const Glyph& g, int src_top, int src_bottom, int dst_top, int dst_bottom, double sx) {
        Glyph out;
        int gw = width(g);
        int ow = int(std::nearbyint(gw * sx));
        for (int y = dst_top; y <= dst_bottom; y++) {
            int sy = src_top + int(std::nearbyint(double(y - dst_top) * (src_bottom - src_top) / (dst_bottom - dst_top)));
            for (int x = 0; x < ow; x++) {
                auto it = g.find({ std::min(gw - 1, int(x / sx)), sy });
                if (it != g.end()) out[{ x, y }] = it->second;
            }
        }
        return out;
    }

    // Every letter the car names use (docs/calspeed_research.md#car-name-art).
    std::map<char, Glyph> letters(const std::map<std::string, Word>& words) {
        struct Source { char c; const char* word; int x0, x1; };
        static const Source sources[] = {
            { 'P', "PICKUP", 25, 39 }, { 'M', "MOBSTER", 18, 35 }, { 'F', "FORMULA", 17, 29 }, { 'H', "HATCHBACK", 4, 18 },
            { 'C', "MUSCLE", 87, 103 }, { 'S', "SEDAN", 27, 41 }, { 'E', "EXOTIC", 29, 41 }, { 'D', "DEW", 66, 80 },
            { 'B', "BANDIT", 27, 42 }, { 'T', "TAXI", 41, 54 }, { '4', "4X4", 44, 57 }, { 'V', "VAN", 42, 59 },
            { 'a', "SEDAN", 71, 83 }, { 'c', "MUSCLE", 48, 59 }, { 'd', "SEDAN", 57, 69 }, { 'e', "MUSCLE", 67, 79 },
            { 'h', "HATCHBACK", 56, 67 }, { 'i', "CRUISER", 65, 68 }, { 'k', "HATCHBACK", 112, 123 }, { 'l', "MUSCLE", 62, 65 },
            { 'm', "COMPACT", 46, 64 }, { 'n', "SEDAN", 86, 97 }, { 'o', "COMPACT", 31, 44 }, { 'p', "PICKUP", 89, 102 },
            { 'r', "CRUISER", 41, 48 }, { 's', "MUSCLE", 34, 46 }, { 't', "BANDIT", 93, 100 }, { 'u', "MUSCLE", 20, 32 },
            { 'x', "TAXI", 67, 79 },
        };
        std::map<char, Glyph> g;
        for (const Source& s : sources) g[s.c] = cut(words.at(s.word), s.x0, s.x1);
        const Glyph& C = g['C'];
        const Glyph& S = g['S'];
        g['I'] = g['l'];
        g['O'] = unite({ C, hflip(C) });
        g['8'] = unite({ S, hflip(S) });
        Glyph terminal;
        for (int x = 10; x < 14; x++) {
            if (S.count({ x, 8 })) terminal[{ x, 9 }] = edge;
        }
        g['6'] = unite({ region(S, 0, 999, 0, 8), region(S, 0, 8, 9, 11), region(g['8'], 0, 999, 12, 99), terminal });
        g['G'] = unite({ C, region(hflip(C), width(C) / 2, 999, 14, 99) });
        g['q'] = hflip(g['p']);
        g['T'] = drop_corner(g['T'], 13, 9);
        g['V'] = drop_corner(g['V'], 15, 11);
        std::map<int, uint8_t> lower = row_colors(g, "aceonsu"), caps = row_colors(g, "CDEFHPS");
        Glyph bar;
        for (int y = 14; y <= 16; y++) {
            for (int x = 8; x <= 12; x++) bar[{ x, y }] = caps.count(y) ? caps[y] : 6;
        }
        for (int x = 8; x <= 10; x++) bar[{ x, 17 }] = edge;
        g['G'] = unite({ g['G'], bar });
        const Glyph &t = g['t'], &r = g['r'], &l = g['l'], &i = g['i'], &p = g['p'];
        Glyph f_edges;
        for (int x = 1; x <= 5; x++) f_edges[{ x, 23 }] = edge;
        for (int y = 20; y <= 22; y++) f_edges[{ 5, y }] = edge;
        g['f'] = unite({ shift(region(r, 0, 999, 10, 13), 1, -5), region(t, 0, 999, 10, 13), region(t, 0, 5, 14, 19),
                         shift(region(l, 0, 999, 8, 22), 1, 0), f_edges });
        Glyph i_stem;
        for (const auto& [q, v] : i) {
            if (q.second != 23) i_stem[q] = v;
        }
        g['j'] = normalize(unite({ i_stem, region(p, 0, 4, 23, 26), shift(region(p, 0, 4, 26, 27), -3, 0) }));
        g['v'] = rescale(g['V'], 6, 23, 10, 23, 0.78);
        g['z'] = from_mask({ "###########", "###########", "##########", "     #####", "    #####", "    ####", "   ####",
                             "  #####", " #####", "#####", "###########", "###########", "###########" }, 10, lower);
        return g;
    }

    // The bank's 128x32 words and its shared palette. False if it isn't laid out as expected.
    bool read_words(const std::vector<uint8_t>& bank, std::map<std::string, Word>& words, std::vector<uint8_t>& palette) {
        if (bank.size() < 0x28) return false;
        uint32_t tex = be32(bank, 8), pals = be32(bank, 12), ntex = be32(bank, 20), npal = be32(bank, 24);
        if (uint64_t(tex) + uint64_t(ntex) * 0x20 > bank.size() || uint64_t(pals) + uint64_t(npal) * 0x18 > bank.size()) return false;
        for (uint32_t k = 0; k < ntex; k++) {
            size_t rec = tex + size_t(k) * 0x20;
            int tw = (bank[rec + 16] << 8) | bank[rec + 17], th = (bank[rec + 18] << 8) | bank[rec + 19];
            uint16_t pi = uint16_t((bank[rec + 22] << 8) | bank[rec + 23]);
            uint32_t texels = be32(bank, rec + 24);
            if (tw != w || th != h || bank[rec + 20] != 0x01 || pi >= npal || size_t(texels) + w * h > bank.size()) continue;
            Word word;
            word.palette = be32(bank, pals + size_t(pi) * 0x18 + 20);
            if (size_t(word.palette) + 512 > bank.size()) continue;
            word.px.resize(size_t(w * h));
            for (int y = 0; y < h; y++) memcpy(&word.px[size_t(y * w)], &bank[texels + size_t((h - 1 - y) * w)], w);
            words[std::string(reinterpret_cast<const char*>(&bank[rec]), strnlen(reinterpret_cast<const char*>(&bank[rec]), 16))] = std::move(word);
        }
        auto sportster = words.find("SPORTSTER");
        if (sportster == words.end()) return false;
        palette.assign(bank.begin() + sportster->second.palette, bank.begin() + sportster->second.palette + 512);
        return true;
    }

    // The source words letters() cuts from, all on the shared palette.
    bool words_usable(const std::map<std::string, Word>& words) {
        static const char* used[] = { "PICKUP", "MOBSTER", "FORMULA", "HATCHBACK", "MUSCLE", "SEDAN", "EXOTIC", "DEW",
                                      "BANDIT", "TAXI", "4X4", "VAN", "CRUISER", "COMPACT" };
        auto sportster = words.find("SPORTSTER");
        for (const char* name : used) {
            auto it = words.find(name);
            if (it == words.end() || it->second.palette != sportster->second.palette) return false;
        }
        return true;
    }

    std::vector<uint8_t> texels_of(const Glyph& g, int gw) {
        std::vector<uint8_t> out(size_t(w * h), 0);
        int ox = (w - gw) / 2;
        for (const auto& [p, v] : g) {
            int x = p.first + ox, y = p.second;
            if (x >= 0 && x < w && y >= 0 && y < h) out[size_t((h - 1 - y) * w + x)] = v; // rows bottom-up
        }
        return out;
    }

    // `text` set a texel apart, a space 6 wide.
    Glyph compose(const std::map<char, Glyph>& g, const std::string& text, int& gw) {
        Glyph out;
        int x = 0;
        for (char c : text) {
            auto it = g.find(c);
            if (c == ' ' || it == g.end()) { x += 6; continue; }
            out = unite({ out, shift(it->second, x, 0) });
            x += width(it->second) + 1;
        }
        gw = x - 1;
        return out;
    }

    bool build(const std::vector<uint8_t>& rom, std::vector<rush2::car2049::NameLogo>& logos) {
        std::vector<uint8_t> bank, palette;
        std::map<std::string, Word> words;
        if (!rush2::calspeed::read_asset(rom, logo_asset, bank) || !read_words(bank, words, palette) || !words_usable(words)) {
            return false;
        }
        std::map<char, Glyph> g = letters(words);
        for (int n = 0; n < car_count; n++) {
            rush2::car2049::NameLogo& l = logos[size_t(n)];
            std::string text = logo_texts[n];
            if (text[0] == '@') {
                const Word& word = words.at(text.substr(1));
                l.texels.resize(size_t(w * h));
                for (int y = 0; y < h; y++) memcpy(&l.texels[size_t((h - 1 - y) * w)], &word.px[size_t(y * w)], w);
            }
            else {
                int gw = 0;
                Glyph name = compose(g, text, gw);
                l.texels = texels_of(name, gw);
            }
            l.palette = palette;
        }
        return true;
    }
}

std::vector<rush2::car2049::NameLogo> rush2::calspeed::name_logos(const std::vector<uint8_t>& rom) {
    std::vector<rush2::car2049::NameLogo> logos(car_count);
    for (int n = 0; n < car_count; n++) logos[size_t(n)].text = car_names[n];
    uint64_t key = rom_source(rom) ^ (uint64_t(cache_version) << 56);
    std::vector<uint8_t> blob;
    constexpr size_t each = size_t(w * h) + 512;
    if (rush2::track_cache::load_blob("calspeed_name_logos", key, blob) && blob.size() == each * car_count) {
        for (int n = 0; n < car_count; n++) {
            auto at = blob.begin() + ptrdiff_t(each * n);
            logos[size_t(n)].texels.assign(at, at + w * h);
            logos[size_t(n)].palette.assign(at + w * h, at + ptrdiff_t(each));
        }
        return logos;
    }
    std::vector<rush2::car2049::NameLogo> built = logos;
    if (!build(rom, built)) {
        fprintf(stderr, "[CalSpeed] Couldn't build the car name logos from asset %d; lettering them instead\n", logo_asset);
        return logos;
    }
    blob.clear();
    for (const auto& l : built) {
        blob.insert(blob.end(), l.texels.begin(), l.texels.end());
        blob.insert(blob.end(), l.palette.begin(), l.palette.end());
    }
    rush2::track_cache::save_blob("calspeed_name_logos", key, blob);
    return built;
}
