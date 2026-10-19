// The game's Rush 2049 arcade converters (src/arcade2049/) outside the game: builds the two mods into folders, so
// their output can be checked against tools/arcade2049/track_mod.py and car_mod.py (and with tools/track_mod_test.bat)
// without a game run. Built and run by tools/arcade2049/cpp_test/build.bat:
//
//   build.bat [OUT_DIR] [DATA_DIR]     (OUT_DIR default tmp/arcade/cpp; DATA_DIR default ProjectR's sf2049se folder)
//
// Writes OUT_DIR/rush2049_arcade/ and OUT_DIR/rush2049_arcade_cars/ and prints each converter's log and time.

#include <chrono>
#include <cstdio>
#include <fstream>

// The game links stb_image through RT64; here it is compiled in.
#define STB_IMAGE_IMPLEMENTATION
#include "stb/stb_image.h"

#include "arcade2049.h"
#include "texture_upscale.h"

namespace a49 = rush2::arcade2049;

static bool write_mod(const std::filesystem::path& dir, const a49::ModFiles& files) {
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    for (const auto& [name, bytes] : files) {
        std::filesystem::path p = dir / std::filesystem::u8path(name);
        std::filesystem::create_directories(p.parent_path(), ec);
        std::ofstream f(p, std::ios::binary);
        f.write(reinterpret_cast<const char*>(bytes.data()), std::streamsize(bytes.size()));
        if (!f) return false;
    }
    return true;
}

int main(int argc, char** argv) {
    std::filesystem::path out = argc > 1 ? std::filesystem::u8path(argv[1]) : std::filesystem::path("tmp/arcade/cpp");
    std::filesystem::path data = argc > 2 ? std::filesystem::u8path(argv[2]) : a49::default_data_dir();
    if (!a49::is_data_dir(data)) {
        fprintf(stderr, "%s is not the arcade data folder\n", rush2::upscale::path_utf8(data).c_str());
        return 1;
    }
    int status = 0;
    for (int which = 0; which < 2; which++) {
        auto t0 = std::chrono::steady_clock::now();
        a49::ModFiles files;
        std::string log;
        bool ok = which == 0 ? a49::build_track_mod(data, "assets/arcade2049", files, log)
                             : a49::build_car_mod(data, files, log);
        double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        const char* id = which == 0 ? a49::track_mod_id : a49::car_mod_id;
        printf("== %s: %s in %.1f s, %zu files\n%s", id, ok ? "built" : "FAILED", s, files.size(), log.c_str());
        if (!ok || !write_mod(out / id, files)) status = 1;
    }
    return status;
}
