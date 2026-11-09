// The game's PlayStation SF Rush converter (src/sfrush_ps1/) outside the game: builds the mod into a folder, so its
// output can be checked (tools/track_mod_test.bat, tools/track_mod_check.py, tools/glb_view.py) without a game run.
// Built and run by tools/sfrush_ps1/cpp_test/build.bat:
//
//   build.bat [OUT_DIR] [DISC]   (OUT_DIR default tmp/ps1/cpp; DISC the .cue or data track .bin, default
//                                 SFRUSH_PS1_DISC)
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
    bool ok = ps1::build_mod(disc, std::filesystem::path("assets") / "sfrush_ps1", files, log);
    double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    printf("== %s: %s in %.1f s, %zu files\n%s", ps1::mod_id, ok ? "built" : "FAILED", s, files.size(), log.c_str());
    if (!ok) return 1;
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
