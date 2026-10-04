// Standalone test of src/track2049_convert.cpp: converts Rush 2049 race tracks 1-6 for Rush 2 slot 2 (HAWAII), with
// static_paths on and off, writes the files track.py writes and compares them byte for byte with the Python output.
//
//     out\track2049_test.exe [repo root]       (build.bat builds it and runs it from the repo root)
//
// References: tools/rush2049/out/trackN (python track.py) for static_paths on, and
// tools/rush2049/cpp_test/out/ref_nostatic/trackN (python tools/rush2049/cpp_test/make_ref.py) for off.
// C++ output goes to tools/rush2049/cpp_test/out/{static,nostatic}/trackN.
// ROMs: RUSH2049_ROM (default %LOCALAPPDATA%\Rush2Recompiled\rush2049.z64) and RUSH2_ROM (default
// <repo>/rush2.us.recomp.z64, which has Rush 2's main code uncompressed at 0x01000000 for the asset table).

#include <chrono>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "track2049_convert.h"

namespace fs = std::filesystem;

namespace {
    bool read_file(const fs::path& path, std::vector<uint8_t>& out) {
        std::ifstream f(path, std::ios::binary);
        if (!f) {
            return false;
        }
        out.assign(std::istreambuf_iterator<char>(f), {});
        return true;
    }

    void write_file(const fs::path& path, const std::vector<uint8_t>& data) {
        std::ofstream f(path, std::ios::binary);
        f.write((const char*)data.data(), (std::streamsize)data.size());
    }

    // Compares one file with its reference; prints and returns false on a difference.
    bool compare(const fs::path& mine, const fs::path& ref) {
        std::vector<uint8_t> a, b;
        if (!read_file(ref, b)) {
            printf("    %s: no reference %s\n", mine.filename().string().c_str(), ref.string().c_str());
            return false;
        }
        read_file(mine, a);
        if (a == b) {
            return true;
        }
        size_t i = 0;
        while (i < a.size() && i < b.size() && a[i] == b[i]) {
            i++;
        }
        printf("    %s DIFFERS: size 0x%zX vs 0x%zX, first difference at 0x%zX\n", mine.filename().string().c_str(),
               a.size(), b.size(), i);
        return false;
    }

    uint32_t be32(const std::vector<uint8_t>& d, size_t o) {
        return ((uint32_t)d[o] << 24) | ((uint32_t)d[o + 1] << 16) | ((uint32_t)d[o + 2] << 8) | d[o + 3];
    }
}

int main(int argc, char** argv) {
    fs::path repo = argc > 1 ? argv[1] : ".";
    const char* env49 = getenv("RUSH2049_ROM");
    const char* env2 = getenv("RUSH2_ROM");
    const char* appdata = getenv("LOCALAPPDATA");
    fs::path rom49_path = env49 ? fs::path(env49) : fs::path(appdata ? appdata : "") / "Rush2Recompiled" / "rush2049.z64";
    fs::path rom2_path = env2 ? fs::path(env2) : repo / "rush2.us.recomp.z64";

    std::vector<uint8_t> rom49, rom2;
    if (!read_file(rom49_path, rom49) || !read_file(rom2_path, rom2)) {
        printf("Can't read %s or %s\n", rom49_path.string().c_str(), rom2_path.string().c_str());
        return 1;
    }

    // Rush 2's asset table (0x800C185C) in the recomp ROM's uncompressed main code.
    auto asset_rom = [&](int index) { return be32(rom2, 0x01000000 + 0x800C185C - 0x800539E0 + index * 4); };
    std::set<std::string> shared;
    if (!rush2::track2049::rush2_shared_model_names(rom2, asset_rom(0x12), asset_rom(0x14), shared)) {
        printf("Can't read Rush 2's shared model names\n");
        return 1;
    }
    printf("Rush 2 shared models: %zu\n", shared.size());

    fs::path test_dir = repo / "tools" / "rush2049" / "cpp_test" / "out";
    int matched = 0, total = 0;
    for (bool static_paths : { true, false }) {
        for (int k = 1; k <= 6; k++) {
            std::string track = "track" + std::to_string(k);
            fs::path dir = test_dir / (static_paths ? "static" : "nostatic") / track;
            fs::path ref = static_paths ? repo / "tools" / "rush2049" / "out" / track : test_dir / "ref_nostatic" / track;
            fs::create_directories(dir);

            rush2::track2049::ConvertedTrack t;
            std::string error;
            auto start = std::chrono::steady_clock::now();
            bool ok = rush2::track2049::convert_track(rom49, k, "HAWAII", shared, static_paths, t, error);
            double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
            total++;
            if (!ok) {
                printf("%s static_paths=%d: FAILED: %s\n", track.c_str(), static_paths, error.c_str());
                continue;
            }
            write_file(dir / "geometry.bin", t.geometry);
            write_file(dir / "placement.bin", t.placement);
            write_file(dir / "collision.bin", t.collision);
            write_file(dir / "path.bin", t.path);
            write_file(dir / "pathb.bin", t.path_backward);
            write_file(dir / "pvs.bin", t.pvs);
            {
                // Text mode, like Python's open(..., 'w').
                std::ofstream tables(dir / "tables.txt");
                char line[64];
                snprintf(line, sizeof(line), "fog %02x%02x%02x\npvs_count %d\n", t.fog[0], t.fog[1], t.fog[2], t.pvs_count);
                tables << line;
            }
            {
                // The animated textures, in the text texanim.py's describe() writes.
                std::ofstream f(dir / "texanim.txt");
                char line[256];
                for (const auto& fb : t.tex_anims.flipbooks) {
                    uint32_t period;
                    memcpy(&period, &fb.period, 4);
                    snprintf(line, sizeof(line), "flip %s settimg %x start %d %s period %08x frames ", fb.target.c_str(),
                             fb.settimg, fb.start, fb.forward ? "forward" : "backward", period);
                    f << line;
                    for (size_t i = 0; i < fb.frames.size(); i++) {
                        snprintf(line, sizeof(line), "%s%x", i ? "," : "", fb.frames[i]);
                        f << line;
                    }
                    f << "\n";
                }
                for (const auto& sc : t.tex_anims.scrolls) {
                    snprintf(line, sizeof(line), "scroll %s %s position %d wrap %d speed %d rate %d tiles ",
                             sc.target.c_str(), sc.t ? "t" : "s", sc.position, sc.wrap, sc.speed, sc.rate);
                    f << line;
                    for (size_t i = 0; i < sc.tile_sizes.size(); i++) {
                        snprintf(line, sizeof(line), "%s%x:%08x", i ? "," : "", sc.tile_sizes[i].offset,
                                 sc.tile_sizes[i].w1);
                        f << line;
                    }
                    f << "\n";
                }
            }
            bool same = true;
            for (const char* name : { "geometry.bin", "placement.bin", "collision.bin", "path.bin", "pathb.bin", "pvs.bin",
                                      "tables.txt", "texanim.txt" }) {
                same &= compare(dir / name, ref / name);
            }
            matched += same;
            printf("%s static_paths=%d: %.1f ms, geometry 0x%zX, placement 0x%zX, collision 0x%zX: %s\n", track.c_str(),
                   static_paths, ms, t.geometry.size(), t.placement.size(), t.collision.size(),
                   same ? "identical" : "DIFFERENT");
        }
    }
    printf("%d of %d conversions byte-identical to the Python output\n", matched, total);
    return matched == total ? 0 : 1;
}
