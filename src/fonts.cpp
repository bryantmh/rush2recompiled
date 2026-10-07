// High-resolution fonts (Settings > Graphics > Fonts).
//
// The redrawn glyph sheets and race HUD numbers live in a texture pack that ships with the game
// (assets/rush2_hires_fonts.rtz, built by tools/build_font_pack.py). It's copied into the mods folder at startup so
// the mod system loads it like any other pack, and the option turns it on and off with the same texture pack override
// the mod menu uses.
//
// RT64 matches replacements by hashing the texture's TMEM contents, but the game's 2D image loader (func_80055F78)
// sets up images with a wrapping tile whose mask is bigger than the image (a 59-row glyph sheet in a 64-row tile, a
// 7-row "MPH" label in an 8-row tile). RT64 then hashes the rows past the image too, and those hold whatever an
// earlier texture left in TMEM, so the same image got a different hash nearly every time. Clamping T when the loaded
// height is smaller than the mask makes RT64 hash only the rows that were loaded; the game never samples past them,
// so the clamp doesn't change what's drawn. Font sheets are clamped on S as well (the redrawn sheets are sampled
// clamped), but other images aren't: the race HUD's digits sample past their tile's width on purpose (a second tile
// reads each row at double S to reach the high nibble of every CI8 texel).
//
// Set RUSH2_DUMP_FONT_TABLES=<path> to dump the font tables the pack is built from (see dump_font_tables), and
// RUSH2_DUMP_IMAGES=<path> to dump every 2D image the game loads (see dump_image_load).

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <set>
#include <string>
#include <vector>

#include "recomp.h"
#include "librecomp/mods.hpp"
#include "recompui/renderer.h"
#include "util/file.h"

#include "rush2.h"

namespace {
    constexpr uint32_t dl_2d_cursor = 0x80125ABC; // Next free command in the 2D display list.
    constexpr uint32_t g_settimg = 0xFD;
    constexpr uint32_t g_loadtlut = 0xF0;
    constexpr uint32_t g_settile = 0xF5;
    constexpr uint32_t g_settilesize = 0xF2;
    constexpr uint32_t g_tx_loadtile = 7;
    constexpr uint32_t g_im_fmt_i = 4;
    constexpr uint32_t clamp_s = 2u << 8;  // G_TX_CLAMP in cms.
    constexpr uint32_t clamp_t = 2u << 18; // G_TX_CLAMP in cmt.
    constexpr uint32_t font_table = 0x800BEE88; // Fonts used by the text drawer (func_80072A38), 0x10 bytes each.
    constexpr int max_fonts = 32;

    const std::string font_pack_id = "rush2_hires_fonts";
    const std::string font_pack_filename = font_pack_id + ".rtz";

    std::atomic<bool> hires_fonts_enabled = false;
    int32_t font_load_start = 0;

    std::vector<char> read_file(const std::filesystem::path& path) {
        std::ifstream file{ path, std::ios::binary };
        return { std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>() };
    }
}

// Writes every font's pages (glyph sheet pixels and character rectangles) as JSON to the path in
// RUSH2_DUMP_FONT_TABLES, for tools/build_font_pack.py. The tables and sheets are static data, so one dump at the first
// image load is enough.
static void dump_font_tables(uint8_t* rdram) {
    static bool done = false;
    const char* path = getenv("RUSH2_DUMP_FONT_TABLES");
    if (done || path == nullptr) {
        return;
    }
    done = true;

    FILE* f = fopen(path, "w");
    if (f == nullptr) {
        return;
    }
    fprintf(f, "[\n");
    bool first_font = true;
    for (int id = 0; id < max_fonts; id++) {
        // Font: page count at 0x9, page array at 0xC. Unused ids have no pages.
        int32_t font = (int32_t)(font_table + id * 0x10);
        uint32_t pages = (uint32_t)MEM_W(0xC, font);
        int page_count = MEM_BU(0x9, font);
        if (pages < 0x80000000u || pages >= 0x80800000u || page_count == 0 || page_count > 16) {
            continue;
        }
        fprintf(f, "%s{\"id\":%d,\"pages\":[", first_font ? "" : ",\n", id);
        first_font = false;
        for (int p = 0; p < page_count; p++) {
            // Page: first and last character, sheet width and height, sheet pixels (I4), character rectangles.
            int32_t page = (int32_t)(pages + p * 0x14);
            int first = MEM_HU(0x0, page);
            int last = MEM_HU(0x2, page);
            int width = MEM_H(0x6, page);
            int height = MEM_H(0x8, page);
            int32_t image = MEM_W(0xC, page);
            int32_t glyphs = MEM_W(0x10, page);
            fprintf(f, "%s{\"first\":%d,\"last\":%d,\"w\":%d,\"h\":%d,\"glyphs\":[", p ? "," : "", first, last,
                width, height);
            // Character rectangle: x0, y0, x1, y1 in the sheet. y1 = 0 for characters the page doesn't draw.
            for (int c = 0; c <= last - first; c++) {
                int32_t g = glyphs + c * 8;
                fprintf(f, "%s[%d,%d,%d,%d]", c ? "," : "", MEM_H(0, g), MEM_H(2, g), MEM_H(4, g), MEM_H(6, g));
            }
            fprintf(f, "],\"pixels\":\"");
            for (int i = 0; i < width * height / 2; i++) {
                fprintf(f, "%02x", MEM_BU(i, image));
            }
            fprintf(f, "\"}");
        }
        fprintf(f, "]}");
    }
    fprintf(f, "\n]\n");
    fclose(f);
}

// Display list commands from start to end as a JSON array of [word0, word1], adding 4 KiB (enough to fill TMEM) from
// every texture image they set to images.
static std::string dump_commands(uint8_t* rdram, int32_t start, int32_t end, std::string& images) {
    std::string out = "[";
    for (int32_t cmd = start; cmd < end; cmd += 8) {
        uint32_t word0 = (uint32_t)MEM_W(0, cmd);
        uint32_t word1 = (uint32_t)MEM_W(4, cmd);
        char text[32];
        snprintf(text, sizeof(text), "%s[%u,%u]", cmd == start ? "" : ",", word0, word1);
        out += text;
        if ((word0 >> 24) == g_settimg && (word1 & 0x00FFFFFF) < 0x800000 - 0x1000) {
            snprintf(text, sizeof(text), "%s\"%u\":\"", images.empty() ? "" : ",", word1);
            images += text;
            int32_t address = (int32_t)(0x80000000u | (word1 & 0x00FFFFFF));
            for (int i = 0; i < 0x1000; i++) {
                snprintf(text, sizeof(text), "%02x", MEM_BU(i, address));
                images += text;
            }
            images += "\"";
        }
    }
    return out + "]";
}

// Appends each distinct image load as a JSON line to the path in RUSH2_DUMP_IMAGES, for tools/build_font_pack.py to
// find and hash images to redraw: the commands func_80055F78 wrote, the commands of the last palette load before them,
// the commands that followed up to the next image load (CI images can get their palette there, before the draw), and
// the texture images all of them point to. Each load is written at the next one, once the commands after it exist.
static void dump_image_load(uint8_t* rdram, int32_t start, int32_t end) {
    static std::set<std::string> seen;
    static int32_t pending_start = 0, pending_end = 0;
    static const char* path = getenv("RUSH2_DUMP_IMAGES");
    if (path == nullptr) {
        return;
    }

    if (pending_start != 0 && start > pending_end && start - pending_end < 0x800) {
        std::string images;
        std::string line = "{\"cmds\":" + dump_commands(rdram, pending_start, pending_end, images);
        for (int32_t cmd = pending_start - 8; cmd >= pending_start - 0x2000; cmd -= 8) {
            if (((uint32_t)MEM_W(0, cmd) >> 24) == g_loadtlut) {
                line += ",\"tlut\":" + dump_commands(rdram, cmd - 0x20, cmd + 8, images);
                break;
            }
        }
        line += ",\"after\":" + dump_commands(rdram, pending_end, start, images);
        line += ",\"images\":{" + images + "}}\n";
        if (seen.insert(line).second) {
            FILE* f = fopen(path, "a");
            if (f != nullptr) {
                fputs(line.c_str(), f);
                fclose(f);
            }
        }
    }
    pending_start = start;
    pending_end = end;
}

// The small font (font 0, one 64 x 17 page from 0x16) has no ':' (its rectangle is empty), which times need. It gets one
// in the sheet's unused space right of the digits: two full-intensity texels in a 1 x 5 cell, as its '.' (x = 51) has
// one. The high-resolution pack draws the sheet's ':' from its squared glyphs like the others.
static void add_small_font_colon(uint8_t* rdram) {
    constexpr int colon_x = 40, colon_y = 12, height = 5;
    int32_t font = (int32_t)font_table;
    uint32_t pages = (uint32_t)MEM_W(0xC, font);
    if (MEM_BU(0x9, font) != 1 || pages < 0x80000000u || pages >= 0x80800000u) {
        return;
    }
    int32_t page = (int32_t)pages;
    int first = MEM_HU(0x0, page), last = MEM_HU(0x2, page);
    int width = MEM_H(0x6, page), sheet_height = MEM_H(0x8, page);
    if (first > ':' || last < ':' || width != 64 || sheet_height < colon_y + height) {
        return;
    }
    int32_t glyph = MEM_W(0x10, page) + (':' - first) * 8;
    if (MEM_H(6, glyph) != 0) {
        return;
    }
    int32_t image = MEM_W(0xC, page);
    for (int y = colon_y; y < colon_y + height; y++) {
        if (MEM_BU((y * width + colon_x) / 2, image) != 0) {
            return; // not the unused space it was
        }
    }
    for (int y : { colon_y + 1, colon_y + 3 }) {
        MEM_B((y * width + colon_x) / 2, image) = (int8_t)0xF0; // even x: the high nibble
    }
    MEM_H(0, glyph) = colon_x;
    MEM_H(2, glyph) = colon_y;
    MEM_H(4, glyph) = colon_x;
    MEM_H(6, glyph) = colon_y + height - 1;
}

extern "C" {

// func_80055F78 entry: loads a 2D image into the 2D display list.
void rush2_font_load_begin(uint8_t* rdram, recomp_context* ctx) {
    add_small_font_colon(rdram);
    dump_font_tables(rdram);
    font_load_start = MEM_W(0, (int32_t)dl_2d_cursor);
}

// func_80055F78 exit. Clamps font sheet tiles, and other tiles whose height is smaller than their mask.
void rush2_font_load_end(uint8_t* rdram, recomp_context* ctx) {
    int32_t end = MEM_W(0, (int32_t)dl_2d_cursor);
    if (font_load_start == 0 || end < font_load_start || end - font_load_start > 0x400) {
        return;
    }
    dump_image_load(rdram, font_load_start, end);
    if (!hires_fonts_enabled) {
        return;
    }

    for (int32_t cmd = font_load_start; cmd < end; cmd += 8) {
        uint32_t word0 = (uint32_t)MEM_W(0, cmd);
        uint32_t word1 = (uint32_t)MEM_W(4, cmd);
        uint32_t tile = (word1 >> 24) & 0x7;
        if ((word0 >> 24) != g_settile || tile == g_tx_loadtile) {
            continue;
        }

        // Font sheets (the only intensity images) are clamped on both axes: their glyph rectangles never reach past
        // the sheet, and the redrawn sheets are made to be sampled clamped.
        if (((word0 >> 21) & 0x7) == g_im_fmt_i) {
            MEM_W(4, cmd) = (int32_t)(word1 | clamp_s | clamp_t);
            continue;
        }

        // The tile's size comes from the next SetTileSize for it (coordinates in 10.2 fixed point).
        for (int32_t size_cmd = cmd + 8; size_cmd < end; size_cmd += 8) {
            uint32_t size0 = (uint32_t)MEM_W(0, size_cmd);
            uint32_t size1 = (uint32_t)MEM_W(4, size_cmd);
            if ((size0 >> 24) != g_settilesize || ((size1 >> 24) & 0x7) != tile) {
                continue;
            }
            uint32_t height = ((size1 & 0xFFF) - (size0 & 0xFFF)) / 4 + 1;
            uint32_t maskt = (word1 >> 14) & 0xF;
            if (maskt != 0 && height < (1u << maskt)) {
                word1 |= clamp_t;
            }
            MEM_W(4, cmd) = (int32_t)word1;
            break;
        }
    }
}

}

void rush2::install_font_pack() {
    std::filesystem::path source = recompui::file::get_program_path() / "assets" / font_pack_filename;
    std::filesystem::path mods_dir = recomp::mods::get_mods_directory();
    std::filesystem::path dest = mods_dir / font_pack_filename;

    std::vector<char> pack = read_file(source);
    if (pack.empty()) {
        fprintf(stderr, "High-resolution font pack missing from %s\n", source.string().c_str());
        return;
    }

    // Replace the installed copy whenever it differs, so pack updates reach existing installs.
    if (read_file(dest) != pack) {
        std::error_code ec;
        std::filesystem::create_directories(mods_dir, ec);
        std::ofstream out{ dest, std::ios::binary };
        out.write(pack.data(), (std::streamsize)pack.size());
    }
}

void rush2::set_hires_fonts_enabled(bool enabled) {
    hires_fonts_enabled = enabled;
    if (enabled) {
        recompui::renderer::secondary_enable_texture_pack(font_pack_id);
    }
    else {
        recompui::renderer::secondary_disable_texture_pack(font_pack_id);
    }
}
