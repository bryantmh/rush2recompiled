// Offline test of src/rush1/car1_decals.cpp: builds each car's decal colour maps from the files make_car_ref.py wrote to
// out/cars and compares them with the Python prototype's (tools/rush1/cardecal.py).
//
//     out\car_decals_test.exe <repo root>        (car_decals.bat builds it, writes the inputs and runs it)

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>

#include "car1_decals.h"

namespace fs = std::filesystem;

static bool read_all(const fs::path& path, std::vector<uint8_t>& out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    out.assign(std::istreambuf_iterator<char>(f), {});
    return true;
}

int main(int argc, char** argv) {
    fs::path dir = fs::path(argc > 1 ? argv[1] : ".") / "tools" / "rush1" / "cpp_test" / "out" / "cars";
    std::vector<uint8_t> stripe;
    if (!read_all(dir / "stripe.bin", stripe)) {
        printf("no inputs in %s (run make_car_ref.py)\n", dir.string().c_str());
        return 2;
    }
    int bad = 0;
    for (int c = 0; c < rush2::car1decals::car_count; c++) {
        std::string name = rush2::car1decals::cars[c].name;
        std::vector<uint8_t> r1, r2, ref;
        if (!read_all(dir / (name + "_r1.bin"), r1) || !read_all(dir / (name + "_r2.bin"), r2) ||
            !read_all(dir / (name + "_ref.bin"), ref)) {
            printf("%s: missing files\n", name.c_str());
            bad++;
            continue;
        }
        rush2::car1decals::Pattern p;
        if (!rush2::car1decals::build(c, r1, r2, stripe, p)) {
            printf("%s: build failed\n", name.c_str());
            bad++;
            continue;
        }
        std::vector<uint8_t> got;
        int lit = 0;
        for (int n = 1; n <= 6; n++) {
            const auto& pn = p.panel[n];
            got.push_back((uint8_t)pn.w);
            got.push_back((uint8_t)pn.h);
            if (pn.w) {
                got.insert(got.end(), pn.full.begin(), pn.full.end());
                got.insert(got.end(), pn.lod.begin(), pn.lod.end());
                for (uint8_t v : pn.full) lit += v != 0;
            }
        }
        if (got != ref) {
            size_t at = 0;
            while (at < got.size() && at < ref.size() && got[at] == ref[at]) at++;
            printf("%s: DIFFERS (C++ %zu bytes, Python %zu; first difference at %zu)\n", name.c_str(), got.size(), ref.size(), at);
            bad++;
        }
        else {
            printf("%s: match, %d decal texels\n", name.c_str(), lit);
        }
    }
    printf(bad ? "%d PROBLEMS\n" : "ALL MATCH\n", bad);
    return bad ? 1 : 0;
}
