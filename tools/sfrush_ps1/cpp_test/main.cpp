// The game's PlayStation SF Rush converter (src/sfrush_ps1/) outside the game: builds the mod into a folder, so its
// output can be checked (tools/track_mod_test.bat, tools/track_mod_check.py, tools/glb_view.py) without a game run.
// Built and run by tools/sfrush_ps1/cpp_test/build.bat:
//
//   build.bat [OUT_DIR] [DISC]   (OUT_DIR default tmp/ps1/cpp; DISC any image ps1_disc.cpp reads, default
//                                 SFRUSH_PS1_DISC)
//
// RUSH1_LANES_DIR: a folder with the N64 SF Rush's lane files lanes58.bin, lanes59.bin and lanes60.bin (ROM assets
// 58-60, decompressed: tools/rush1/r1.py Rush1().asset(58 + t)), as the game passes them with the N64 ROM in; without it
// the PS1's own lanes are used.
//
// Writes OUT_DIR/sfrush_ps1/ and prints the converter's log and time.

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>

#define STB_IMAGE_IMPLEMENTATION
#include "stb/stb_image.h"

#include "sfrush_ps1.h"
#include "texture_upscale.h"

namespace ps1 = rush2::sfrush_ps1;

int main(int argc, char** argv) {
    std::filesystem::path out = argc > 1 ? std::filesystem::u8path(argv[1]) : std::filesystem::path("tmp/ps1/cpp");
    const char* env = std::getenv("SFRUSH_PS1_DISC");
    std::filesystem::path disc_path = argc > 2 ? std::filesystem::u8path(argv[2]) : std::filesystem::u8path(env ? env : "");
    ps1::Disc disc;
    std::string error;
    if (!ps1::is_disc(disc_path) || !disc.open(disc_path, error)) {
        fprintf(stderr, "%s is not the PS1 SF Rush disc %s\n", rush2::upscale::path_utf8(disc_path).c_str(), error.c_str());
        return 1;
    }
    auto t0 = std::chrono::steady_clock::now();
    ps1::ModFiles files;
    std::string log;
    ps1::N64Lanes n64;
    if (const char* dir = std::getenv("RUSH1_LANES_DIR"); dir && *dir) {
        for (int t = 0; t < 3; t++) {
            n64[size_t(t)] = rush2::upscale::read_file(std::filesystem::u8path(dir) / ("lanes" + std::to_string(58 + t) + ".bin"));
        }
    }
    // The N64 Embarcadero as the game's install hands it over (tools/sfrush_ps1/n64_scene.py writes it from a model dump).
    ps1::N64Scene scene;
    if (const char* path = std::getenv("SFRUSH_PS1_N64_SCENE"); path && *path) {
        std::vector<uint8_t> d = rush2::upscale::read_file(std::filesystem::u8path(path));
        size_t o = 0;
        auto u32 = [&]() { uint32_t v = 0; if (o + 4 <= d.size()) std::memcpy(&v, &d[o], 4); o += 4; return v; };
        uint32_t images = u32();
        for (uint32_t i = 0; i < images && o < d.size(); i++) {
            ps1::N64Scene::Image im;
            im.width = u32(); im.height = u32(); im.wrap_s = int(u32()); im.wrap_t = int(u32());
            size_t n = size_t(im.width) * im.height * 4;
            if (o + n > d.size()) break;
            im.rgba.assign(d.begin() + o, d.begin() + o + n);
            o += n;
            scene.images.push_back(std::move(im));
        }
        uint32_t tris = u32();
        for (uint32_t i = 0; i < tris && o + 3 * 24 + 4 <= d.size(); i++) {
            ps1::N64Scene::Tri t{};
            for (int v = 0; v < 3; v++) {
                std::memcpy(t.p[v], &d[o], 12); std::memcpy(t.uv[v], &d[o + 12], 8); std::memcpy(t.color[v], &d[o + 20], 4);
                o += 24;
            }
            std::memcpy(&t.image, &d[o], 4); o += 4;
            scene.tris.push_back(t);
        }
        printf("N64 scene: %zu images, %zu triangles\n", scene.images.size(), scene.tris.size());
    }
    bool ok = ps1::build_mod(disc, std::filesystem::path("assets") / "sfrush_ps1", files, log, n64, scene);
    double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    printf("== %s: %s in %.1f s, %zu files\n%s", ps1::mod_id, ok ? "built" : "FAILED", s, files.size(), log.c_str());
    if (!ok) return 1;
    // The disc's music (any image format): each track's length and an FNV-1a hash of its samples, to compare formats.
    for (const ps1::AudioTrack& t : ps1::audio_tracks(disc_path)) {
        std::vector<uint8_t> pcm;
        bool read = ps1::read_audio(t, pcm);
        uint64_t h = 1469598103934665603ull;
        for (uint8_t c : pcm) h = (h ^ c) * 1099511628211ull;
        printf("audio track %d: %llu bytes, %s, hash %016llx\n", t.number, (unsigned long long)t.length,
               read ? "read" : "READ FAILED", (unsigned long long)h);
    }
    std::filesystem::path dir = out / ps1::mod_id;
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    for (const auto& [name, bytes] : files) {
        std::filesystem::path p = dir / std::filesystem::u8path(name);
        std::filesystem::create_directories(p.parent_path(), ec);
        std::ofstream f(p, std::ios::binary);
        f.write(reinterpret_cast<const char*>(bytes.data()), std::streamsize(bytes.size()));
        if (!f) return 1;
    }
    return 0;
}
