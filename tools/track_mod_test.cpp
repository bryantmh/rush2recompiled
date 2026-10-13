// Converts a custom track folder (track.glb + track.toml) outside the game and writes the Rush 2 files it becomes, plus
// the converter's report, for checking with tools/track_mod_check.py. Built by tools/track_mod_test.bat. The track select
// art (logo, route preview) is made in the game only (src/track_mod/track_mod_menu.cpp).
//
//     track_mod_test.exe <track folder> <output folder> [prefix]      (prefix: the host slot's, default HAWAII)

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "track_mod.h"

#define STB_IMAGE_IMPLEMENTATION
#include "stb/stb_image.h"

static std::vector<uint8_t> read_all(const std::filesystem::path& p) {
    std::ifstream f(p, std::ios::binary);
    return std::vector<uint8_t>(std::istreambuf_iterator<char>(f), {});
}

static void write_all(const std::filesystem::path& p, const std::vector<uint8_t>& d) {
    std::ofstream f(p, std::ios::binary);
    f.write(reinterpret_cast<const char*>(d.data()), std::streamsize(d.size()));
}

int main(int argc, char** argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: track_mod_test <track folder> <output folder> [prefix]\n");
        return 2;
    }
    std::filesystem::path in = argv[1], outdir = argv[2];
    std::string prefix = argc > 3 ? argv[3] : "HAWAII";
    std::filesystem::create_directories(outdir);
    std::vector<uint8_t> glb = read_all(in / "track.glb");
    std::vector<uint8_t> toml = read_all(in / "track.toml");
    rush2::track_mod::Settings settings;
    rush2::track_mod::Report settings_report;
    rush2::track_mod::parse_settings(std::string(toml.begin(), toml.end()), settings, settings_report);
    rush2::track_mod::Extras extras;
    if (!settings.logo.empty()) extras.logo = read_all(in / settings.logo);
    if (!settings.diorama.empty()) extras.diorama = read_all(in / settings.diorama);
    rush2::track_mod::Converted out;
    rush2::track_mod::Report report;
    bool ok = rush2::track_mod::convert(glb, std::string(toml.begin(), toml.end()), prefix, out, report, extras);
    std::string text = report.text();
    fputs(text.c_str(), stdout);
    write_all(outdir / "report.txt", std::vector<uint8_t>(text.begin(), text.end()));
    if (!ok) {
        printf("FAILED\n");
        return 1;
    }
    write_all(outdir / "geometry.bin", out.track.geometry);
    write_all(outdir / "placement.bin", out.track.placement);
    write_all(outdir / "collision.bin", out.track.collision);
    write_all(outdir / "path.bin", out.track.path);
    write_all(outdir / "path_backward.bin", out.track.path_backward);
    printf("OK: wrote %s\n", outdir.string().c_str());
    return 0;
}
