// Writes a Rush 2 track (geometry container + placement file) as the placed-track .glb the model dump makes
// (rush2::catalog::write_glb, docs/asset_catalog.md), outside the game. Used by tools/trackview/stock.py to show the
// SF Rush and California Speed tracks' models. Built by tools/trackview/track_glb.bat.
//
//     track_glb.exe GEOMETRY PLACEMENT OUT.glb [TITLE]
#define STB_IMAGE_IMPLEMENTATION
#include "stb/stb_image.h"

#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "asset_catalog.h"

static std::vector<uint8_t> read_all(const char* path) {
    std::ifstream f(path, std::ios::binary);
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

int main(int argc, char** argv) {
    if (argc < 4) {
        fprintf(stderr, "usage: track_glb GEOMETRY PLACEMENT OUT.glb [TITLE]\n");
        return 2;
    }
    rush2::catalog::Container c;
    std::string error;
    if (!rush2::catalog::read_container(read_all(argv[1]), c, error)) {
        fprintf(stderr, "can't read the geometry: %s\n", error.c_str());
        return 1;
    }
    std::vector<rush2::catalog::Placement> placements = rush2::catalog::read_placement(read_all(argv[2]));
    std::vector<uint8_t> glb = rush2::catalog::write_glb(c, argc > 4 ? argv[4] : "track", placements);
    std::ofstream(argv[3], std::ios::binary).write(reinterpret_cast<const char*>(glb.data()), std::streamsize(glb.size()));
    printf("%zu models, %zu textures, %zu placements, %zu bytes\n", c.models.size(), c.textures.size(), placements.size(), glb.size());
    return 0;
}
