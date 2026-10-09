// Converts Rush 2049 car N to a Rush 2 car asset and writes it: out\car_dump.exe N out.bin [NAME]
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <vector>

#include "rush2049_rom.h"
#include "track2049_convert.h"

int main(int argc, char** argv) {
    std::string path = std::string(getenv("LOCALAPPDATA")) + "\\Rush2Recompiled\\rush2049.z64";
    std::ifstream f(path, std::ios::binary);
    std::vector<uint8_t> rom((std::istreambuf_iterator<char>(f)), {});
    std::vector<uint8_t> out;
    for (int i : { 77, 88, 101 }) {
        bool ok = rush2::rom2049::read_file(rom, i, out);
        printf("rom %zu file %d: %d %zu\n", rom.size(), i, (int)ok, out.size());
    }
    std::string err;
    if (!rush2::track2049::convert_car(*rush2::rom2049::n64_source(std::make_shared<const std::vector<uint8_t>>(rom)), argc > 1 ? atoi(argv[1]) : 1, argc > 3 ? argv[3] : "PICKUP", out, err)) {
        printf("error: %s\n", err.c_str());
        return 1;
    }
    std::ofstream o(argc > 2 ? argv[2] : "car.bin", std::ios::binary);
    o.write((const char*)out.data(), (std::streamsize)out.size());
    printf("%zu bytes\n", out.size());
    return 0;
}
