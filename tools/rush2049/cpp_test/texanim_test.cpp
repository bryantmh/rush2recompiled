// Standalone test of the animated textures (src/track2049_texanim.cpp and ConvertedTrack::tex_anims from
// src/track2049_convert.cpp) for Rush 2049 race tracks 1-6 in Rush 2 slot 2 (HAWAII).
//
//     out\texanim_test.exe [png dir] [seconds]     (texanim_build.bat builds and runs it)
//
// Per track:
// 1. Patch sites, checked against the converted file independently of the converter's lookup: every flip-book's
//    G_SETTIMG and every scroll's G_SETTILESIZE lies in the texture-load list range and in the load list of the
//    texture record named by the target; each frame address lies in the texel data; how many model-list G_DL calls
//    reach each target list; and no other load in the file loads a target's texels (that copy would not animate).
// 2. Runtime: the geometry is "loaded" into a fake RDRAM the way Rush 2 loads it (func_80077B38 rebases the load
//    lists), the slot record is set up as func_800A4C98 leaves it, and texanim_tick runs at 60 ticks per second. The
//    frame sequence and step interval of every flip-book and the tile origin of every scroll are printed and checked
//    against 2049's update at 30 frames per second.
// 3. PNGs in [png dir] (default: the session scratchpad's texanim folder): trackK_flip.png (a row per flip-book, a
//    column per 0.1 s: each target texture decoded through its patched load list) and trackK_scroll.png (a row per
//    scroll, a column per 2049 frame: the tile window at the patched origin), 4x.
// 4. A corrupted patch site makes the runtime refuse to animate.
// ROM: RUSH2049_ROM (default %LOCALAPPDATA%\Rush2Recompiled\rush2049.z64), RUSH2_ROM (default <repo>/rush2.us.recomp.z64).

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "miniz.h"
#include "recomp.h"
#include "track2049.h"
#include "track2049_convert.h"

namespace fs = std::filesystem;
using rush2::track2049::TexAnims;

namespace {
    int raced = 0;

    constexpr uint32_t rdram_size = 0x800000;
    constexpr uint32_t load_base = 0x80400000;
    constexpr uint32_t slot_records = 0x8010C258;
    constexpr uint32_t geometry_record = 0x8010C400;
    constexpr uint32_t track_id = 0x8010C3F0;
    constexpr int record_index = 5;

    bool read_file(const fs::path& path, std::vector<uint8_t>& out) {
        std::ifstream f(path, std::ios::binary);
        if (!f) {
            return false;
        }
        out.assign(std::istreambuf_iterator<char>(f), {});
        return true;
    }

    uint32_t be32(const std::vector<uint8_t>& d, size_t o) {
        return ((uint32_t)d[o] << 24) | ((uint32_t)d[o + 1] << 16) | ((uint32_t)d[o + 2] << 8) | d[o + 3];
    }

    uint32_t rd32(uint8_t* rdram, uint32_t a) {
        return (uint32_t)MEM_W(0, (int32_t)a);
    }

    void wr32(uint8_t* rdram, uint32_t a, uint32_t v) {
        MEM_W(0, (int32_t)a) = (int32_t)v;
    }

    uint8_t rd8(uint8_t* rdram, uint32_t a) {
        return (uint8_t)MEM_BU(0, (int32_t)a);
    }

    int failures = 0;
    void check(bool ok, const char* fmt, const std::string& a, uint32_t b = 0) {
        if (!ok) {
            printf("    FAIL: ");
            printf(fmt, a.c_str(), b);
            printf("\n");
            failures++;
        }
    }

    // Texture record of the converted geometry by name: data, flags, w, h.
    struct Tex {
        uint32_t data = 0, flags = 0;
        int w = 0, h = 0;
        bool found = false;
    };
    Tex texture(const std::vector<uint8_t>& g, const std::string& name) {
        Tex t;
        uint32_t table = be32(g, 8), count = be32(g, 20);
        for (uint32_t i = 0; i < count; i++) {
            size_t r = table + i * 0x20;
            std::string n((const char*)&g[r], strnlen((const char*)&g[r], 16));
            if (n.substr(0, 15) == name.substr(0, 15)) {
                t.data = be32(g, r + 0x18);
                t.flags = be32(g, r + 0x1C);
                t.w = (g[r + 0x10] << 8) | g[r + 0x11];
                t.h = (g[r + 0x12] << 8) | g[r + 0x13];
                t.found = true;
                return t;
            }
        }
        return t;
    }

    // The load list [start, G_ENDDL].
    std::pair<uint32_t, uint32_t> list_span(const std::vector<uint8_t>& g, uint32_t start) {
        uint32_t o = start;
        while (o + 8 <= g.size() && g[o] != 0xDF) {
            o += 8;
        }
        return { start, o };
    }

    // Model-list G_DL commands (outside the load-list range) calling `list`.
    int dl_calls(const std::vector<uint8_t>& g, uint32_t list) {
        uint32_t ts = be32(g, 28), te = be32(g, 32);
        int n = 0;
        for (uint32_t o = te; o + 8 <= g.size(); o += 8) {
            if (g[o] == 0xDE && (be32(g, o + 4) & 0xFFFFFF) == list) {
                n++;
            }
        }
        (void)ts;
        return n;
    }

    // G_SETTIMG commands anywhere (8-byte aligned) with w1 == addr, outside [skip_start, skip_end].
    int other_loads(const std::vector<uint8_t>& g, uint32_t addr, uint32_t skip_start, uint32_t skip_end) {
        int n = 0;
        for (uint32_t o = 0x28; o + 8 <= g.size(); o += 8) {
            if (o >= skip_start && o <= skip_end) {
                continue;
            }
            if (g[o] == 0xFD && be32(g, o + 4) == addr) {
                n++;
            }
        }
        return n;
    }

    // Loads the converted geometry at load_base as Rush 2 does for the load lists (func_80077B38) and sets up the
    // slot record func_800A4C98 leaves for the track geometry.
    void load(uint8_t* rdram, const std::vector<uint8_t>& g) {
        memset(rdram, 0, rdram_size);
        for (size_t o = 0; o + 4 <= g.size(); o += 4) {
            wr32(rdram, load_base + (uint32_t)o, be32(g, o));
        }
        uint32_t start = be32(g, 28), end = be32(g, 32);
        for (uint32_t o = start; o < end; o += 8) {
            uint32_t a = load_base + o;
            uint32_t w0 = rd32(rdram, a);
            uint8_t op = (uint8_t)(w0 >> 24);
            if ((op & 0xC0) == 0x40 || (op & 0xC0) == 0x80 || (op >= 9 && op < 0x40) || (op >= 0xC0 && op < 0xD6)) {
                continue;
            }
            uint8_t next = (uint8_t)(rd32(rdram, a + 8) >> 24);
            if (op == 0xFD || (op == 0xE1 && next == 0x04)) {
                uint32_t w1 = rd32(rdram, a + 4);
                wr32(rdram, a + 4, ((w1 + load_base) & 0xFFFFFF) | (w1 & 0x0F000000));
            }
        }
        wr32(rdram, geometry_record, record_index);
        uint32_t r = slot_records + record_index * 12;
        wr32(rdram, r, (uint32_t)(0x33 + rush2::track2049::host_slot) << 24 | 0x0A0000);
        wr32(rdram, r + 4, load_base);
        MEM_B(0, (int32_t)track_id) = rush2::track2049::host_slot;
    }

    // ---------------------------------------------------------------------------------------------------------
    // Decoding a texture through its (patched) load list in the fake RDRAM.

    struct Image {
        int w = 0, h = 0;
        std::vector<uint32_t> px; // 0xAABBGGRR
    };

    uint32_t rgba16(uint16_t c) {
        uint32_t r = ((c >> 11) & 31) * 255 / 31, g = ((c >> 6) & 31) * 255 / 31, b = ((c >> 1) & 31) * 255 / 31;
        return (c & 1 ? 0xFF000000u : 0) | (b << 16) | (g << 8) | r;
    }

    // Decodes tile 0 of the load list at `list` (geometry-relative), offset by its tile origin, with w x h texels.
    Image decode(uint8_t* rdram, uint32_t list, int w, int h) {
        Image im;
        im.w = w;
        im.h = h;
        im.px.assign((size_t)w * h, 0xFF808080);
        uint32_t img_addr = 0, tlut = 0, last = 0;
        int fmt = 0, siz = 0, pal = 0, uls = 0, ult = 0;
        for (uint32_t a = load_base + list;; a += 8) {
            uint32_t w0 = rd32(rdram, a), w1 = rd32(rdram, a + 4);
            uint8_t op = (uint8_t)(w0 >> 24);
            if (op == 0xDF) {
                break;
            }
            if (op == 0xFD) {
                last = w1;
            }
            else if (op == 0xF3 || op == 0xF4) {
                if (img_addr == 0) img_addr = last;
            }
            else if (op == 0xF0) {
                tlut = last;
            }
            else if (op == 0xF5 && ((w1 >> 24) & 7) == 0) {
                fmt = (w0 >> 21) & 7;
                siz = (w0 >> 19) & 3;
                pal = (w1 >> 20) & 15;
            }
            else if (op == 0xF2 && ((w1 >> 24) & 7) == 0) {
                uls = (w0 >> 12) & 0xFFF;
                ult = w0 & 0xFFF;
            }
        }
        int bits = 4 << siz;
        for (int y = 0; y < h; y++) {
            for (int x = 0; x < w; x++) {
                int sx = ((x + (uls >> 2)) % w + w) % w, sy = ((y + (ult >> 2)) % h + h) % h;
                size_t bit = ((size_t)sy * w + sx) * bits;
                uint32_t a = 0x80000000u | (img_addr & 0xFFFFFF);
                uint32_t v = 0;
                for (int i = 0; i < (bits + 7) / 8; i++) {
                    v = (v << 8) | rd8(rdram, a + (uint32_t)(bit / 8) + i);
                }
                if (bits == 4) {
                    v = (bit & 4) ? (v & 15) : (v >> 4);
                }
                uint32_t c = 0;
                if (fmt == 2) { // CI
                    uint32_t index = bits == 4 ? pal * 16 + v : v;
                    uint32_t ta = 0x80000000u | (tlut & 0xFFFFFF);
                    c = rgba16((uint16_t)((rd8(rdram, ta + index * 2) << 8) | rd8(rdram, ta + index * 2 + 1)));
                }
                else if (fmt == 0 && bits == 16) {
                    c = rgba16((uint16_t)v);
                }
                else if (fmt == 0 && bits == 32) {
                    c = ((v & 0xFF) << 24) | (((v >> 8) & 0xFF) << 16) | (((v >> 16) & 0xFF) << 8) | (v >> 24);
                }
                else if (fmt == 3) { // IA
                    uint32_t i8, a8;
                    if (bits == 4) { i8 = ((v >> 1) & 7) * 255 / 7; a8 = (v & 1) * 255; }
                    else if (bits == 8) { i8 = (v >> 4) * 17; a8 = (v & 15) * 17; }
                    else { i8 = v >> 8; a8 = v & 0xFF; }
                    c = (a8 << 24) | (i8 << 16) | (i8 << 8) | i8;
                }
                else { // I
                    uint32_t i8 = bits == 4 ? v * 17 : v & 0xFF;
                    c = 0xFF000000u | (i8 << 16) | (i8 << 8) | i8;
                }
                // Composite over grey so alpha shows.
                uint32_t al = c >> 24;
                auto mix = [&](int sh) { return (((c >> sh) & 0xFF) * al + 0x60 * (255 - al)) / 255; };
                im.px[(size_t)y * w + x] = 0xFF000000u | (mix(16) << 16) | (mix(8) << 8) | mix(0);
            }
        }
        return im;
    }

    // A grid of images (rows x columns), each cell scaled 4x and padded to the largest image, 2-pixel gaps.
    bool write_grid(const fs::path& path, const std::vector<std::vector<Image>>& rows) {
        int cw = 0, ch = 0, cols = 0;
        for (const auto& r : rows) {
            cols = std::max(cols, (int)r.size());
            for (const auto& im : r) {
                cw = std::max(cw, im.w * 4);
                ch = std::max(ch, im.h * 4);
            }
        }
        if (rows.empty() || cols == 0) {
            return false;
        }
        int W = cols * (cw + 2), H = (int)rows.size() * (ch + 2);
        std::vector<uint8_t> out((size_t)W * H * 3, 0x20);
        for (size_t ri = 0; ri < rows.size(); ri++) {
            for (size_t ci = 0; ci < rows[ri].size(); ci++) {
                const Image& im = rows[ri][ci];
                for (int y = 0; y < im.h * 4; y++) {
                    for (int x = 0; x < im.w * 4; x++) {
                        uint32_t c = im.px[(size_t)(y / 4) * im.w + x / 4];
                        size_t o = ((size_t)(ri * (ch + 2) + y) * W + ci * (cw + 2) + x) * 3;
                        out[o] = c & 0xFF;
                        out[o + 1] = (c >> 8) & 0xFF;
                        out[o + 2] = (c >> 16) & 0xFF;
                    }
                }
            }
        }
        size_t len = 0;
        void* png = tdefl_write_image_to_png_file_in_memory(out.data(), W, H, 3, &len);
        if (png == nullptr) {
            return false;
        }
        std::ofstream f(path, std::ios::binary);
        f.write((const char*)png, (std::streamsize)len);
        mz_free(png);
        return true;
    }

    float from_bits(uint32_t b) {
        float f;
        memcpy(&f, &b, 4);
        return f;
    }
}

int rush2::track2049::race_track() {
    return raced;
}

int main(int argc, char** argv) {
    fs::path png_dir = argc > 1 ? fs::path(argv[1]) :
        fs::path("C:/Users/Bryant/AppData/Local/Temp/claude/c--Users-Bryant-Documents-Code-rush2-recomp/"
                 "c23db066-4ec2-4ff7-89b6-027c926211e3/scratchpad/texanim");
    double seconds = argc > 2 ? atof(argv[2]) : 3.0;
    fs::create_directories(png_dir);
    const char* env49 = getenv("RUSH2049_ROM");
    const char* env2 = getenv("RUSH2_ROM");
    const char* appdata = getenv("LOCALAPPDATA");
    fs::path rom49_path = env49 ? fs::path(env49) : fs::path(appdata ? appdata : "") / "Rush2Recompiled" / "rush2049.z64";
    fs::path rom2_path = env2 ? fs::path(env2) : fs::path("rush2.us.recomp.z64");
    std::vector<uint8_t> rom49, rom2;
    if (!read_file(rom49_path, rom49) || !read_file(rom2_path, rom2)) {
        printf("Can't read %s or %s\n", rom49_path.string().c_str(), rom2_path.string().c_str());
        return 1;
    }
    auto asset_rom = [&](int index) { return be32(rom2, 0x01000000 + 0x800C185C - 0x800539E0 + index * 4); };
    std::set<std::string> shared;
    if (!rush2::track2049::rush2_shared_model_names(rom2, asset_rom(0x12), asset_rom(0x14), shared)) {
        printf("Can't read Rush 2's shared model names\n");
        return 1;
    }

    std::vector<uint8_t> rdram_buf(rdram_size);
    uint8_t* rdram = rdram_buf.data();
    const float vblank = from_bits(0x3C888889);
    const float frame49 = 2.0f * vblank;
    const float tick = 1.0f / 60.0f;

    for (int k = 1; k <= 6; k++) {
        rush2::track2049::ConvertedTrack t;
        std::string error;
        if (!rush2::track2049::convert_track(rom49, k, "HAWAII", shared, true, t, error)) {
            printf("track %d: conversion failed: %s\n", k, error.c_str());
            failures++;
            continue;
        }
        const auto& g = t.geometry;
        const TexAnims& an = t.tex_anims;
        uint32_t txld_start = be32(g, 28), txld_end = be32(g, 32);
        printf("track %d: %zu flip-books, %zu scrolls\n", k, an.flipbooks.size(), an.scrolls.size());

        // 1. Patch sites.
        for (const auto& f : an.flipbooks) {
            Tex tt = texture(g, f.target);
            auto span = list_span(g, tt.data);
            check(tt.found, "%s: no texture record", f.target);
            check(f.settimg >= txld_start && f.settimg < txld_end, "%s: G_SETTIMG 0x%X outside the load lists",
                  f.target, f.settimg);
            check(g[f.settimg] == 0xFD, "%s: no G_SETTIMG at 0x%X", f.target, f.settimg);
            check(f.settimg >= span.first && f.settimg < span.second, "%s: 0x%X not in its load list", f.target,
                  f.settimg);
            bool first = true;
            for (uint32_t o = span.first; o < f.settimg; o += 8) {
                first &= g[o] != 0xFD;
            }
            check(first, "%s: 0x%X isn't the list's first G_SETTIMG", f.target, f.settimg);
            check(be32(g, f.settimg + 4) == f.frames[f.start], "%s: frame start isn't the target's texels (0x%X)",
                  f.target, be32(g, f.settimg + 4));
            int texel_bytes = tt.w * tt.h / 2;  // at least 4 bits per texel
            for (uint32_t v : f.frames) {
                check(v >= 0x28 && v + texel_bytes <= txld_start, "%s: frame texels 0x%X outside the texel data",
                      f.target, v);
            }
            int calls = dl_calls(g, tt.data);
            // Loads of the target's texels by lists that aren't flip-book targets: those copies would stay still.
            int others = other_loads(g, be32(g, f.settimg + 4), span.first, span.second);
            for (const auto& o : an.flipbooks) {
                if (o.settimg != f.settimg && be32(g, o.settimg + 4) == be32(g, f.settimg + 4)) {
                    others--;
                }
            }
            printf("  flip   %-16s list %06X  settimg %06X  %2zu frames  %s %.3fs  G_DL calls %d  non-animated loads of "
                   "its texels %d\n", f.target.c_str(), tt.data, f.settimg, f.frames.size(),
                   f.forward ? "fwd" : "bwd", f.period, calls, others);
        }
        for (const auto& s : an.scrolls) {
            Tex tt = texture(g, s.target);
            auto span = list_span(g, tt.data);
            int n_f2 = 0;
            for (uint32_t o = span.first; o < span.second; o += 8) {
                n_f2 += g[o] == 0xF2;
            }
            check((int)s.tile_sizes.size() == n_f2, "%s: %u G_SETTILESIZE in the list", s.target, n_f2);
            for (const auto& c : s.tile_sizes) {
                check(g[c.offset] == 0xF2 && be32(g, c.offset + 4) == c.w1 && c.offset >= span.first &&
                      c.offset < span.second, "%s: bad G_SETTILESIZE site 0x%X", s.target, c.offset);
            }
            printf("  scroll %-16s list %06X  %zu tiles  %s speed %d per vblank, wrap %d  G_DL calls %d\n",
                   s.target.c_str(), tt.data, s.tile_sizes.size(), s.t ? "t" : "s", s.speed, s.wrap,
                   dl_calls(g, tt.data));
        }

        // 2. Runtime.
        load(rdram, g);
        raced = k;
        rush2::track2049::set_texanim_data(an);
        rush2::track2049::texanim_reset();
        int ticks = (int)(seconds * 60);
        // Expected state, from 2049's update at 30 fps (func_800BD2C8 with 0x8002EB98 = 2): a flip-book's timer
        // starts at its period, loses the frame time each frame and, at <= 0, is set back to the period as the step
        // moves on; a scroll moves speed * 2 every frame (its timer rearms at 1/30 s, so it never waits).
        std::vector<int> cur(an.flipbooks.size());
        std::vector<float> timer(an.flipbooks.size());
        std::vector<std::vector<int>> seq(an.flipbooks.size()), steps(an.flipbooks.size());
        for (size_t j = 0; j < an.flipbooks.size(); j++) {
            cur[j] = an.flipbooks[j].start;
            timer[j] = an.flipbooks[j].period;
            seq[j].push_back(cur[j]);
        }
        std::vector<int> pos(an.scrolls.size());
        for (size_t j = 0; j < an.scrolls.size(); j++) pos[j] = an.scrolls[j].position;
        std::vector<std::vector<int>> origins(an.scrolls.size());
        std::vector<std::vector<Image>> flip_rows(an.flipbooks.size()), scroll_rows(an.scrolls.size());
        std::vector<bool> flip_ok(an.flipbooks.size(), true), scroll_ok(an.scrolls.size(), true);
        for (int i = 0; i <= ticks; i++) {
            if (i > 0) {
                rush2::track2049::texanim_tick(rdram, tick);
            }
            bool frame_ran = i > 0 && i % 2 == 0;
            for (size_t j = 0; j < an.flipbooks.size(); j++) {
                const auto& f = an.flipbooks[j];
                int count = (int)f.frames.size();
                if (frame_ran) {
                    timer[j] = timer[j] - frame49;
                    if (timer[j] <= 0.0f) {
                        timer[j] = f.period;
                        cur[j] = f.forward ? (cur[j] + 1) % count : (cur[j] + count - 1) % count;
                        seq[j].push_back(cur[j]);
                        steps[j].push_back(i);
                    }
                }
                uint32_t w1 = rd32(rdram, load_base + f.settimg + 4);
                uint32_t want = ((f.frames[cur[j]] + load_base) & 0xFFFFFF) | (f.frames[cur[j]] & 0x0F000000);
                flip_ok[j] = flip_ok[j] && w1 == want;
                if (i % 6 == 0 && i <= 6 * 23) {
                    Tex tt = texture(g, f.target);
                    flip_rows[j].push_back(decode(rdram, tt.data, tt.w, tt.h));
                }
            }
            for (size_t j = 0; j < an.scrolls.size(); j++) {
                const auto& s = an.scrolls[j];
                if (frame_ran) {
                    pos[j] += s.speed * 2;
                    if (pos[j] < 0) pos[j] += s.wrap; else if (pos[j] >= s.wrap) pos[j] -= s.wrap;
                }
                // Expected origins per tile: position / 4, halved per tile smaller than the first.
                int size = -1, value = pos[j] >> 2;
                bool moved = i >= 2;
                for (const auto& c : s.tile_sizes) {
                    int extent = (int)(s.t ? (c.w1 & 0xFFF) : ((c.w1 >> 12) & 0xFFF)) + 4;
                    if (size < 0) size = extent;
                    else while (extent < size) { size >>= 1; value >>= 1; }
                    uint32_t w0 = rd32(rdram, load_base + c.offset);
                    int origin = s.t ? (int)(w0 & 0xFFF) : (int)((w0 >> 12) & 0xFFF);
                    scroll_ok[j] = scroll_ok[j] && (!moved || origin == value) && (w0 >> 24) == 0xF2 &&
                                   (s.t ? (w0 & 0x00FFF000) == 0 : (w0 & 0xFFF) == 0);
                }
                if (i % 2 == 0) {
                    uint32_t w0 = rd32(rdram, load_base + s.tile_sizes[0].offset);
                    origins[j].push_back(s.t ? (int)(w0 & 0xFFF) : (int)((w0 >> 12) & 0xFFF));
                    if (i <= 2 * 15) {
                        Tex tt = texture(g, s.target);
                        scroll_rows[j].push_back(decode(rdram, tt.data, tt.w, tt.h));
                    }
                }
            }
        }
        for (size_t j = 0; j < an.flipbooks.size(); j++) {
            const auto& f = an.flipbooks[j];
            check(flip_ok[j], "%s: G_SETTIMG differs from 2049's sequence", f.target);
            std::string str;
            for (size_t q = 0; q < seq[j].size() && q < 14; q++) str += std::to_string(seq[j][q]) + " ";
            int every = steps[j].size() > 1 ? steps[j][1] - steps[j][0] : -1;
            printf("  run    %-16s step every %d ticks (%.3f s), frames %s\n", f.target.c_str(), every, every / 60.0,
                   str.c_str());
        }
        for (size_t j = 0; j < an.scrolls.size(); j++) {
            const auto& s = an.scrolls[j];
            check(scroll_ok[j], "%s: G_SETTILESIZE differs from 2049's sequence", s.target);
            std::string str;
            for (size_t q = 0; q < origins[j].size() && q < 12; q++) str += std::to_string(origins[j][q]) + " ";
            printf("  run    %-16s tile 0 origin %s... (per 1/30 s, 10.2 fixed point)\n", s.target.c_str(),
                   str.c_str());
        }
        if (!an.flipbooks.empty()) {
            write_grid(png_dir / ("track" + std::to_string(k) + "_flip.png"), flip_rows);
        }
        if (!an.scrolls.empty()) {
            write_grid(png_dir / ("track" + std::to_string(k) + "_scroll.png"), scroll_rows);
        }

        // 4. A corrupted site: the runtime must leave the geometry alone.
        if (!an.flipbooks.empty()) {
            load(rdram, g);
            uint32_t a = load_base + an.flipbooks[0].settimg;
            wr32(rdram, a, 0xE7000000);
            rush2::track2049::texanim_reset();
            std::vector<uint8_t> before(rdram_buf);
            for (int i = 0; i < 120; i++) rush2::track2049::texanim_tick(rdram, tick);
            check(before == rdram_buf, "%s: patched despite a corrupted site", an.flipbooks[0].target);
        }
        raced = 0;
    }
    printf("%s (%d failures). PNGs in %s\n", failures ? "FAILED" : "all checks passed", failures,
           png_dir.string().c_str());
    return failures ? 1 : 0;
}
