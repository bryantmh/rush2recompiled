// Tests the image code of texture upscaling (src/texture_upscale_images.cpp, docs/texture_upscaling.md) without the
// game: TMEM decoding, hqx, recoloring palette variants, padding, alpha, DDS mipmaps, running commands and, with a
// work folder argument, writing a texture pack. Build and run from the project root (Linux; the miniz sources need a miniz_export.h defining MINIZ_EXPORT,
// e.g. in tmp/gen):
//
//   M=lib/N64ModernRuntime/thirdparty/miniz
//   gcc -c -O2 lib/hqx/hq2x.c lib/hqx/hq4x.c && mv hq2x.o hq4x.o tmp/
//   clang++ -std=c++20 -Iinclude -Ilib/hqx -Ilib/rt64/src/contrib -I$M -Itmp/gen tools/texture_upscale_test.cpp \
//       src/texture_upscale_images.cpp tmp/hq2x.o tmp/hq4x.o $M/miniz.c $M/miniz_tdef.c $M/miniz_tinfl.c \
//       $M/miniz_zip.c -o tmp/upscale_test
//   tmp/upscale_test [<work folder>]
#define STB_IMAGE_IMPLEMENTATION
#include "stb/stb_image.h"
#include "ddspp/ddspp.h"
#include <cassert>
#include <cstdio>
#include <cmath>
#include <cstring>
#include <thread>
#include "texture_upscale.h"
using namespace rush2::upscale;
static int fails = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

static Image load_png(const std::filesystem::path& path) {
    Image im;
    int w = 0, h = 0, n = 0;
    uint8_t* data = stbi_load(path.string().c_str(), &w, &h, &n, 4);
    if (data == nullptr) return im;
    im.width = uint32_t(w);
    im.height = uint32_t(h);
    im.rgba.assign(data, data + size_t(w) * h * 4);
    stbi_image_free(data);
    return im;
}

// --paint DIR UPSCALE.png: recolors a real paint base's upscale (DIR from tools/rush1/paint_case.py) and writes
// DIR/out_recolor.png and DIR/out_recolor_paint.png (an upscale of gray.png) and DIR/out_preview.png (recolor from
// preview.png: an upscale of the colored dump).
static int paint_case(const std::filesystem::path& dir, const std::filesystem::path& upscale_path) {
    Image gray = load_png(dir / "gray.png"), painted = load_png(dir / "painted.png"), up = load_png(upscale_path);
    FILE* f = fopen((dir / "indices.bin").string().c_str(), "rb");
    if (gray.empty() || painted.empty() || up.empty() || f == nullptr) {
        printf("missing inputs\n");
        return 1;
    }
    std::vector<uint8_t> indices(size_t(gray.width) * gray.height);
    size_t got = fread(indices.data(), 1, indices.size(), f);
    fclose(f);
    if (got != indices.size()) return 1;
    uint32_t scale = std::max<uint32_t>(1, (up.width + gray.width / 2) / gray.width);
    if (up.width != gray.width * scale || up.height != gray.height * scale) {
        up = resize_bilinear(up, gray.width * scale, gray.height * scale);
    }
    write_png(dir / "out_recolor.png", recolor(up, gray, painted, Edge::Wrap, Edge::Wrap));
    write_png(dir / "out_recolor_paint.png", recolor_paint(up, gray, painted, indices, Edge::Wrap, Edge::Wrap));
    Image preview = load_png(dir / "preview.png");
    if (!preview.empty()) write_png(dir / "out_preview.png", recolor(up, preview, painted, Edge::Wrap, Edge::Wrap));
    printf("wrote %s\n", (dir / "out_recolor*.png").string().c_str());
    return 0;
}

int main(int argc, char** argv) {
    if (argc == 4 && strcmp(argv[1], "--paint") == 0) return paint_case(argv[2], argv[3]);
    // RGBA16 8x4 texture at TMEM 0, line = 2 words (16 bytes/row). Odd rows have 32-bit words swapped in pairs.
    uint8_t tmem[4096] = {};
    auto px = [](int x, int y) { return uint16_t(((x * 4) << 11) | ((y * 8) << 6) | (((x + y) & 31) << 1) | ((x + y) & 1)); };
    for (int y = 0; y < 4; y++) for (int x = 0; x < 8; x++) {
        int addr = y * 16 + x * 2;
        if (y & 1) { int row = y * 16, w = (addr - row) / 4; addr = row + (w ^ 1) * 4 + (addr & 3); }
        uint16_t v = px(x, y); tmem[addr] = v >> 8; tmem[addr + 1] = v & 0xFF;
    }
    Image img = decode_tmem(tmem, 0, 2, 0, 2, 0, 0, 8, 4);
    bool ok = true;
    for (int y = 0; y < 4; y++) for (int x = 0; x < 8; x++) {
        uint16_t v = px(x, y); uint32_t r = (v >> 11) & 31; uint8_t* p = &img.rgba[(y * 8 + x) * 4];
        ok &= p[0] == ((r << 3) | (r >> 2)) && p[3] == ((v & 1) ? 255 : 0);
    }
    CHECK(ok);

    // CI4 with an RGBA16 TLUT, palette 1: index i -> entry 16+i at 0x800 + (16+i)*8.
    memset(tmem, 0, sizeof(tmem));
    for (int i = 0; i < 16; i++) { uint16_t c = uint16_t((i << 11) | 1); int a = 0x800 + (16 + i) * 8; tmem[a] = c >> 8; tmem[a + 1] = c & 0xFF; }
    for (int x = 0; x < 16; x += 2) tmem[x / 2] = uint8_t(((x & 15) << 4) | ((x + 1) & 15)); // Row 0: indices 0..15.
    Image ci = decode_tmem(tmem, 2, 0, 0, 1, 1, 2u << 14, 16, 1);
    ok = true;
    for (int x = 0; x < 16; x++) ok &= ci.rgba[x * 4] == ((x << 3) | (x >> 2)) && ci.rgba[x * 4 + 3] == 255;
    CHECK(ok);

    // Keys.
    uint64_t k = content_key(img), back;
    CHECK(parse_key_name(key_name(k) + "_gigapixel-art-2x.png", back) && back == k);
    CHECK(!parse_key_name("README.txt", back));

    // Pad/unpad round trip at 3x nearest.
    Image src; src.width = 5; src.height = 3; src.rgba.resize(60);
    for (int i = 0; i < 60; i++) src.rgba[i] = uint8_t(i * 4);
    Image padded = pad(src, 2, Edge::Wrap, Edge::Mirror);
    CHECK(padded.width == 9 && padded.height == 7);
    CHECK(memcmp(&padded.rgba[(2 * 9 + 2) * 4], &src.rgba[0], 4) == 0);
    CHECK(memcmp(&padded.rgba[(2 * 9 + 0) * 4], &src.rgba[3 * 4], 4) == 0);      // Wrap: x=-2 -> 3.
    CHECK(memcmp(&padded.rgba[(1 * 9 + 2) * 4], &src.rgba[0], 4) == 0);          // Mirror: y=-1 -> 0.
    Image big; big.width = 27; big.height = 21; big.rgba.resize(27 * 21 * 4);
    for (uint32_t y = 0; y < 21; y++) for (uint32_t x = 0; x < 27; x++) memcpy(&big.rgba[(y * 27 + x) * 4], &padded.rgba[((y / 3) * 9 + x / 3) * 4], 4);
    Image un = unpad(big, src, 2);
    CHECK(un.width == 15 && un.height == 9 && memcmp(&un.rgba[0], &src.rgba[0], 4) == 0);

    // DDS.
    Image d; d.width = 64; d.height = 16; d.rgba.assign(64 * 16 * 4, 200);
    auto dds = make_dds(d);
    ddspp::Descriptor desc;
    CHECK(ddspp::decode_header(dds.data(), desc) == ddspp::Success);
    CHECK(desc.numMips == 7 && desc.width == 64 && desc.height == 16 && desc.format == ddspp::R8G8B8A8_UNORM);
    size_t expect = desc.headerSize; for (int m = 0; m < 7; m++) expect += size_t(std::max(64 >> m, 1)) * std::max(16 >> m, 1) * 4;
    CHECK(dds.size() == expect);
    CHECK(ddspp::get_offset(desc, 6, 0) + 4 == dds.size() - desc.headerSize);

    // Alpha restore: cutout original, upscaler returned opaque.
    Image orig; orig.width = 2; orig.height = 1; orig.rgba = { 10, 10, 10, 0, 20, 20, 20, 255 };
    Image up; up.width = 4; up.height = 2; up.rgba.assign(32, 255);
    restore_alpha(up, orig);
    CHECK(up.rgba[3] == 0 && up.rgba[15] == 255);

    // Paletted decode gives the indices too (the CI4 texture above: index x).
    std::vector<uint8_t> indices;
    decode_tmem(tmem, 2, 0, 0, 1, 1, 2u << 14, 16, 1, &indices);
    ok = true;
    for (int x = 0; x < 16; x++) ok &= indices[x] == x;
    CHECK(ok);

    // A car-like CI texture: a body shaded along a 31-step paint ramp (index 1-31), a gray trim stripe and a black
    // outline, under two palettes: red paint and blue paint.
    const uint32_t cw = 64, ch = 32;
    std::vector<uint8_t> car(cw * ch);
    for (uint32_t y = 0; y < ch; y++) for (uint32_t x = 0; x < cw; x++) {
        uint8_t i = uint8_t(1 + ((x + y * 2) * 30 / (cw + ch * 2)));                    // Body shade.
        if (y >= 12 && y < 16) i = 200;                                               // Trim.
        if (x == 0 || y == 0 || x == cw - 1 || y == ch - 1 || ((x / 16 + y / 8) % 3 == 0 && x % 16 == 0)) i = 250; // Lines.
        car[y * cw + x] = i;
    }
    auto paint = [&](int r, int g, int b) {
        Image im; im.width = cw; im.height = ch; im.rgba.resize(cw * ch * 4);
        for (uint32_t i = 0; i < cw * ch; i++) {
            uint8_t idx = car[i], *p = &im.rgba[i * 4];
            if (idx < 32) { float t = 1.0f - (idx - 1) / 31.0f; p[0] = uint8_t(r * t); p[1] = uint8_t(g * t); p[2] = uint8_t(b * t); }
            else if (idx == 200) { p[0] = p[1] = p[2] = 160; }
            else { p[0] = p[1] = p[2] = 10; }
            p[3] = 255;
        }
        return im;
    };
    Image red = paint(248, 40, 40), blue = paint(40, 80, 248);
    CHECK(index_key(car, cw, ch, 1) == index_key(car, cw, ch, 1) && content_key(red) != content_key(blue));

    // HQx sizes, and the sanity check.
    Image hq2 = hq_upscale(red, 2), hq4 = hq_upscale(red, 4);
    CHECK(hq2.width == 128 && hq2.height == 64 && hq4.width == 256 && hq4.height == 128);
    CHECK(matches_original(hq2, red) && matches_original(hq4, red));
    Image noise = hq4;
    for (size_t i = 0; i < noise.rgba.size(); i++) noise.rgba[i] = uint8_t((i * 2654435761u) >> 13);
    CHECK(!matches_original(noise, red));
    CHECK(matches_original(downscale(hq4, 2), red));

    // Recoloring the red upscale to blue lands close to upscaling the blue variant directly.
    auto mean_error = [](const Image& a, const Image& b) {
        double total = 0; for (size_t i = 0; i < a.rgba.size(); i++) if (i % 4 != 3) total += std::abs(int(a.rgba[i]) - int(b.rgba[i]));
        return total / (a.rgba.size() / 4 * 3);
    };
    for (uint32_t scale : { 2u, 4u }) {
        Image direct = hq_upscale(blue, scale);
        Image recolored = recolor(hq_upscale(red, scale), red, blue, Edge::Wrap, Edge::Wrap);
        double error = mean_error(recolored, direct), unchanged = mean_error(hq_upscale(red, scale), direct);
        printf("recolor hq%ux: mean error %.2f (red upscale vs blue: %.2f)\n", scale, error, unchanged);
        CHECK(error < 4.0 && error < unchanged / 10);
        Image same = recolor(hq_upscale(red, scale), red, red, Edge::Wrap, Edge::Wrap);
        CHECK(same.rgba == hq_upscale(red, scale).rgba);
    }

    // A Rush 2 car paint base: the catalog's gray image (entry i as gray i & 31) upscaled, then painted by the game's
    // palette (MAIN red, ACCENT white, CARPALETTE's ramp), against upscaling the painted panel directly. MAIN and
    // ACCENT share grays, black trim (191) is gray 31 and the glass (0) gray 0. hq2x only blends neighbors, which
    // recolor explains exactly; this checks recolor_paint stays close to it (its gain shows on hand-made upscales:
    // tools/rush1/paint_case.py and --paint).
    {
        const uint8_t ramp[32] = { 0, 230, 222, 213, 213, 205, 197, 189, 180, 172, 172, 164, 156, 148, 139, 139, 123,
                                   115, 106, 98, 90, 82, 74, 65, 57, 49, 41, 32, 16, 8, 0, 0 }; // CARPALETTE 0-31
        const uint32_t w = 64, h = 32;
        std::vector<uint8_t> panel(w * h);
        for (uint32_t y = 0; y < h; y++) for (uint32_t x = 0; x < w; x++) {
            uint8_t i = uint8_t(4 + x / 4);                          // MAIN, lighter to darker across
            if (y >= 10 && y < 18 && x >= 6) i += 32;                // an ACCENT band at the same shades
            if (y == 24 || x == 30) i = 191;                         // black trim
            if (y < 7 && x >= 46) i = 0;                             // glass
            if (y >= 27 && x < 10) i = 102;                          // a tail light
            panel[y * w + x] = i;
        }
        auto colors = [&](bool gray_ramp) {
            Image im; im.width = w; im.height = h; im.rgba.resize(w * h * 4);
            for (uint32_t k = 0; k < w * h; k++) {
                uint8_t i = panel[k], *p = &im.rgba[k * 4];
                p[3] = 255;
                if (gray_ramp) { uint8_t g = uint8_t(((i & 31) << 3) | ((i & 31) >> 2)); p[0] = p[1] = p[2] = g; continue; }
                if (i >= 1 && i <= 31) { p[0] = uint8_t(248 * ramp[i] / 230); p[1] = p[2] = uint8_t(40 * ramp[i] / 230); }
                else if (i >= 33 && i <= 63) p[0] = p[1] = p[2] = ramp[i - 32];
                else if (i == 191) p[0] = p[1] = p[2] = 8;
                else if (i == 0) { p[0] = 8; p[1] = 24; p[2] = 49; }
                else { p[0] = 197; p[1] = p[2] = 0; }
            }
            return im;
        };
        Image gray = colors(true), painted = colors(false);
        Image direct = hq_upscale(painted, 2), up = hq_upscale(gray, 2);
        Image old_way = recolor(up, gray, painted, Edge::Wrap, Edge::Wrap);
        Image new_way = recolor_paint(up, gray, painted, panel, Edge::Wrap, Edge::Wrap);
        auto fringes = [&](const Image& a) { // Pixels far off the direct upscale's color.
            int count = 0;
            for (size_t i = 0; i < a.rgba.size(); i += 4) {
                int d = 0; for (int c = 0; c < 3; c++) d = std::max(d, std::abs(int(a.rgba[i + c]) - int(direct.rgba[i + c])));
                count += d > 48;
            }
            return count;
        };
        double e_old = mean_error(old_way, direct), e_new = mean_error(new_way, direct);
        int f_old = fringes(old_way), f_new = fringes(new_way);
        printf("paint base: recolor error %.2f, %d fringe pixels; recolor_paint error %.2f, %d fringe pixels\n", e_old,
               f_old, e_new, f_new);
        // hq2x only blends neighbors, which recolor explains exactly; recolor_paint softens the MAIN-ACCENT edges a
        // little but mustn't fringe.
        CHECK(e_new < 2.5 && f_new <= 4);
        if (argc > 1) {
            std::filesystem::path work = argv[1];
            std::filesystem::create_directories(work);
            write_png(work / "paint_direct.png", direct);
            write_png(work / "paint_recolor.png", old_way);
            write_png(work / "paint_recolor_paint.png", new_way);
        }
    }

    // DDS level 0 reads back.
    Image back_image;
    CHECK(read_dds(make_dds(hq4), back_image) && back_image.width == 256 && back_image.rgba == hq4.rgba);

    // Cancelling a running command.
    std::atomic<bool> cancel = false;
    std::thread canceller([&]() { std::this_thread::sleep_for(std::chrono::milliseconds(200)); cancel = true; });
    auto started = std::chrono::steady_clock::now();
    int cancelled = run_command("sleep 5", &cancel);
    canceller.join();
    CHECK(cancelled == -1 && std::chrono::steady_clock::now() - started < std::chrono::seconds(2));

    // Pack files, in a work folder.
    if (argc > 1) {
        std::filesystem::path work = argv[1];
        std::filesystem::remove_all(work);
        Image result = hq_upscale(red, 4);
        uint64_t key = content_key(red);
        std::filesystem::create_directories(work / "pack" / "textures");
        CHECK(write_png(work / "red_hq4x.png", result));
        CHECK(write_png(work / "blue_recolored_hq4x.png", recolor(result, red, blue, Edge::Wrap, Edge::Wrap)));
        CHECK(write_file(work / "pack" / "textures" / (key_name(key) + ".dds"), make_dds(result)));
        std::string db = pack_database({ { "textures/" + key_name(key) + ".dds", { 0x1234abcdULL, key } } });
        std::string mf = pack_manifest("rush2_custom_textures", "Custom \"Upscaled\" Textures", "d");
        CHECK(write_file(work / "pack" / "rt64.json", std::vector<uint8_t>(db.begin(), db.end())));
        CHECK(write_file(work / "pack" / "mod.json", std::vector<uint8_t>(mf.begin(), mf.end())));
        CHECK(zip_folder(work / "pack", work / "pack.rtz"));
        CHECK(run_command("exit 3") == 3);
    }
    printf("%s (%d failures)\n", fails ? "FAILED" : "OK", fails);
    return fails != 0;
}
