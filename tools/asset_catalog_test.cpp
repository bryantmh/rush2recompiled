// Runs the asset catalog's container reader (src/asset_catalog_walk.cpp, docs/asset_catalog.md) on Rush 2 assets and
// converted Rush 2049 cars outside the game: prints each texture's name, size, format, RT64 hash and content key, and
// writes its PNG and the container's .glb.
//
//   asset_catalog_test OUT_DIR [ASSET_HEX ...] [--car49 N ...]   (no assets: every Rush 2 model container)
//
// Reads RUSH2_ROM (default rush2.us.recomp.z64 in the project root) and, for --car49, RUSH2049_ROM. Build from Git Bash:
//   cmd //c "tools\asset_catalog_test.bat"   (writes tmp\catalog_test\asset_catalog_test.exe)
#define STB_IMAGE_IMPLEMENTATION
#include "stb/stb_image.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "asset_catalog.h"
#include "rush2049_rom.h"
#include "track2049_convert.h"
#include "assets.h"

using namespace rush2::catalog;

static std::vector<uint8_t> read_all(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

static uint32_t be32(const std::vector<uint8_t>& d, size_t o) {
    return (uint32_t(d[o]) << 24) | (uint32_t(d[o + 1]) << 16) | (uint32_t(d[o + 2]) << 8) | d[o + 3];
}

static void report(const std::string& label, const std::vector<uint8_t>& data, const std::filesystem::path& out,
                   const std::vector<uint8_t>& placement = {}) {
    Container c;
    std::string error;
    if (!read_container(data, c, error)) {
        printf("%s: %s\n", label.c_str(), error.c_str());
        return;
    }
    size_t tris = 0;
    for (const Model& m : c.models) for (const Lod& l : m.lods) for (const Mesh& mesh : l.meshes) tris += mesh.vertices.size() / 3;
    printf("%s: %zu models, %zu textures, %zu triangles\n", label.c_str(), c.models.size(), c.textures.size(), tris);
    std::filesystem::path dir = out / label;
    std::filesystem::create_directories(dir);
    for (const Texture& t : c.textures) {
        printf("  %-24s %4ux%-4u fmt %u siz %u tlut %04X %s hash %016llx key %016llx texels %06X %s\n", t.name.c_str(),
               t.width, t.height, t.fmt, t.siz, t.tlut, t.painted ? "painted" : "       ", (unsigned long long)t.hash,
               (unsigned long long)t.key, t.texels, t.models.empty() ? "" : t.models[0].c_str());
        rush2::upscale::write_png(dir / (file_name(t.name) + ".png"), t.image);
    }
    std::vector<uint8_t> glb = write_glb(c, label, read_placement(placement));
    std::ofstream(out / (label + ".glb"), std::ios::binary).write((const char*)glb.data(), glb.size());
}

int main(int argc, char** argv) {
    if (argc < 2) {
        printf("usage: asset_catalog_test OUT_DIR [ASSET_HEX ...] [--car49 N ...]\n");
        return 1;
    }
    std::filesystem::path out = argv[1];
    const char* rom_path = getenv("RUSH2_ROM");
    std::vector<uint8_t> rom = read_all(rom_path ? rom_path : "rush2.us.recomp.z64");
    if (rom.size() < 0x1100000) {
        printf("can't read the Rush 2 recomp ROM\n");
        return 1;
    }
    auto asset_rom = [&](int i) { return be32(rom, 0x01000000 + 0x800C185C - 0x800539E0 + i * 4); };
    auto asset = [&](int i, std::vector<uint8_t>& data) {
        uint32_t o = asset_rom(i);
        bool lz = i == 0x12 || i == 0x13 || (i >= 0x15 && i <= 0x18) || (i >= 0x1D && i <= 0x32);
        return lz ? rush2::track2049::rush2_lz_decompress(rom.data() + o, rom.size() - o, data)
                  : rush2::assets::inflate_raw(rom.data() + o, rom.size() - o, data);
    };
    std::vector<int> assets, cars;
    for (int a = 2; a < argc; a++) {
        if (strcmp(argv[a], "--car49") == 0 && a + 1 < argc) {
            cars.push_back(atoi(argv[++a]));
        }
        else {
            assets.push_back((int)strtol(argv[a], nullptr, 16));
        }
    }
    if (assets.empty() && cars.empty()) {
        for (int i = 0; i < 0x71; i++) assets.push_back(i);
    }
    for (int i : assets) {
        std::vector<uint8_t> data;
        char label[16];
        snprintf(label, sizeof(label), "asset_%02X", i);
        std::vector<uint8_t> placement;
        if (i >= 0x33 && i < 0x3F) asset(i + 12, placement); // The track's placement file, 0x3F + track.
        if (asset(i, data)) report(label, data, out, placement);
    }
    if (!cars.empty()) {
        const char* p49 = getenv("RUSH2049_ROM");
        auto rom49 = std::make_shared<const std::vector<uint8_t>>(read_all(p49 ? p49 : ""));
        auto source = rush2::rom2049::n64_source(rom49);
        for (int k : cars) {
            std::vector<uint8_t> car;
            std::string error;
            if (rush2::track2049::convert_car(*source, k, "CAR" + std::to_string(k), car, error)) {
                report("car49_" + std::to_string(k), car, out);
            }
            else {
                printf("car %d: %s\n", k, error.c_str());
            }
        }
    }
    return 0;
}
