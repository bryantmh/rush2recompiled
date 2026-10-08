// Tests the image code of texture upscaling (src/texture_upscale_images.cpp, docs/texture_upscaling.md) without the
// game: TMEM decoding, padding, alpha, DDS mipmaps, and with arguments a real Real-ESRGAN run, zip extraction and pack
// files. Build and run from the project root (Linux; the miniz sources need a miniz_export.h defining MINIZ_EXPORT,
// e.g. in tmp/gen):
//
//   M=lib/N64ModernRuntime/thirdparty/miniz
//   gcc -c -O2 lib/hqx/hq2x.c lib/hqx/hq4x.c && mv hq2x.o hq4x.o tmp/
//   clang++ -std=c++20 -Iinclude -Ilib/hqx -Ilib/rt64/src/contrib -I$M -Itmp/gen tools/texture_upscale_test.cpp \
//       src/texture_upscale_images.cpp tmp/hq2x.o tmp/hq4x.o $M/miniz.c $M/miniz_tdef.c $M/miniz_tinfl.c \
//       $M/miniz_zip.c -o tmp/upscale_test
//   tmp/upscale_test [<extracted realesrgan-ncnn-vulkan folder> <work folder> [<release zip> [download]]]
//
// With "download", the partial download of the Real-ESRGAN release (only the program and the x4 model) is tested
// against GitHub too.
//
// Without a GPU, Mesa's lavapipe runs Real-ESRGAN: VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.json.
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

int main(int argc, char** argv) {
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

    // Real-ESRGAN on a padded texture, through run_command.
    if (argc > 2) {
        std::filesystem::path esr = argv[1], work = argv[2];
        std::filesystem::remove_all(work);
        std::filesystem::create_directories(work / "in"); std::filesystem::create_directories(work / "out");
        Image tex; tex.width = 32; tex.height = 16; tex.rgba.resize(32 * 16 * 4);
        for (uint32_t y = 0; y < 16; y++) for (uint32_t x = 0; x < 32; x++) { uint8_t* p = &tex.rgba[(y * 32 + x) * 4]; p[0] = (x * 8) & 255; p[1] = y * 16; p[2] = ((x / 4 + y / 4) & 1) ? 220 : 40; p[3] = ((x / 8 + y / 8) & 1) ? 255 : 0; }
        uint64_t key = content_key(tex);
        CHECK(write_png(work / "in" / (key_name(key) + ".png"), pad(tex, 8, Edge::Wrap, Edge::Wrap)));
        std::string line = "\"" + (esr / "realesrgan-ncnn-vulkan").string() + "\" -i \"" + (work / "in").string() + "\" -o \"" + (work / "out").string() + "\" -s 2 -n realesr-animevideov3 -m \"" + (esr / "models").string() + "\" -f png";
        int rc = run_command(line);
        printf("esrgan exit %d\n", rc);
        Image out;
        CHECK(read_image(work / "out" / (key_name(key) + ".png"), out));
        Image result = unpad(out, tex, 8);
        CHECK(result.width == 64 && result.height == 32);
        restore_alpha(result, tex);
        bool binary = true; for (size_t i = 3; i < result.rgba.size(); i += 4) binary &= result.rgba[i] == 0 || result.rgba[i] == 255;
        CHECK(binary);
        CHECK(write_png(work / "result.png", result));
        auto rdds = make_dds(result);
        CHECK(write_file(work / "result.dds", rdds));
        // Zip extraction of the downloaded release.
        if (argc > 3) {
            CHECK(extract_zip(argv[3], work / "unzipped"));
            CHECK(std::filesystem::exists(work / "unzipped" / "models" / "realesr-animevideov3-x2.bin"));
        }
        // Pack files.
        std::filesystem::create_directories(work / "pack" / "textures");
        CHECK(write_file(work / "pack" / "textures" / (key_name(key) + ".dds"), rdds));
        std::string db = pack_database({ { "textures/" + key_name(key) + ".dds", { 0x1234abcdULL, key } } });
        std::string mf = pack_manifest("rush2_custom_textures", "Custom \"Upscaled\" Textures", "d");
        CHECK(write_file(work / "pack" / "rt64.json", std::vector<uint8_t>(db.begin(), db.end())));
        CHECK(write_file(work / "pack" / "mod.json", std::vector<uint8_t>(mf.begin(), mf.end())));
        CHECK(zip_folder(work / "pack", work / "pack.rtz"));
        CHECK(run_command("exit 3") == 3);

        // ESRGAN x4 on the red car, and the blue one recolored from it, against ESRGAN on the blue car.
        std::filesystem::remove_all(work / "in"); std::filesystem::remove_all(work / "out");
        std::filesystem::create_directories(work / "in"); std::filesystem::create_directories(work / "out");
        CHECK(write_png(work / "in" / "red.png", pad(red, 8, Edge::Wrap, Edge::Wrap)));
        CHECK(write_png(work / "in" / "blue.png", pad(blue, 8, Edge::Wrap, Edge::Wrap)));
        line = "\"" + (esr / "realesrgan-ncnn-vulkan").string() + "\" -i \"" + (work / "in").string() + "\" -o \"" + (work / "out").string() + "\" -s 4 -n realesr-animevideov3 -m \"" + (esr / "models").string() + "\" -j 1:1:1 -f png";
        CHECK(run_command(line) == 0);
        Image red_out, blue_out;
        CHECK(read_image(work / "out" / "red.png", red_out) && read_image(work / "out" / "blue.png", blue_out));
        Image red4 = unpad(red_out, red, 8), blue4 = unpad(blue_out, blue, 8);
        CHECK(matches_original(red4, red) && matches_original(downscale(red4, 2), red));
        Image recolored = recolor(red4, red, blue, Edge::Wrap, Edge::Wrap);
        double error = mean_error(recolored, blue4), unchanged = mean_error(red4, blue4);
        printf("recolor esrgan x4: mean error %.2f (red upscale vs blue: %.2f)\n", error, unchanged);
        CHECK(error < 6.0);
        write_png(work / "car_red_x4.png", red4); write_png(work / "car_blue_x4.png", blue4); write_png(work / "car_blue_recolored_x4.png", recolored);

        if (argc > 4 && std::string(argv[4]) == "download") {
            std::vector<std::string> files = { "realesrgan-ncnn-vulkan.exe", "vcomp140.dll", "models/realesr-animevideov3-x4.bin", "models/realesr-animevideov3-x4.param" };
            auto t0 = std::chrono::steady_clock::now();
            bool downloaded = download_zip_files("https://github.com/xinntao/Real-ESRGAN/releases/download/v0.2.5.0/realesrgan-ncnn-vulkan-20220424-windows.zip", files, work / "download");
            printf("partial download %s in %.1f s\n", downloaded ? "ok" : "FAILED", std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count());
            CHECK(downloaded);
            CHECK(std::filesystem::file_size(work / "download" / "realesrgan-ncnn-vulkan.exe") == 6161408);
            CHECK(std::filesystem::file_size(work / "download" / "models" / "realesr-animevideov3-x4.bin") == 1247368);
            CHECK(!std::filesystem::exists(work / "download" / "models" / "realesrgan-x4plus.bin"));
        }
    }
    printf("%s (%d failures)\n", fails ? "FAILED" : "OK", fails);
    return fails != 0;
}
