// Offline test of src/track1_convert.cpp: converts every Rush 1 track and compares the files with the Python
// prototype's (tools/rush1/cpp_test/make_ref.py writes them to out/ref), and builds the track select art on Rush 2's
// asset 3 (out/ref/asset3.bin) to out/asset3_rush1.bin.
//
//     out\track1_test.exe <repo root>          (build.bat builds it, writes the references and runs it)
// The Rush 1 ROM comes from RUSH1_ROM or E:\Emulation\roms\n64\San Francisco Rush - Extreme Racing.z64.

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>

#include "track1.h"

namespace fs = std::filesystem;

// The parts of src/rush1_rom.cpp the converter needs, without the UI.
namespace {
    constexpr uint32_t main_rom = 0x7A7930;
    constexpr uint32_t asset_table = 0x800C7C1C;
    std::shared_ptr<const std::vector<uint8_t>> main_data;

    bool lz(const uint8_t* src, size_t size, std::vector<uint8_t>& out) {
        out.clear();
        size_t i = 0;
        int pos = 1;
        while (i < size) {
            uint8_t flags = src[i++];
            for (int bit = 0; bit < 8; bit++) {
                if (flags & (1 << bit)) {
                    if (i >= size) return false;
                    out.push_back(src[i++]);
                    pos = (pos + 1) & 0xFFF;
                    continue;
                }
                if (i + 2 > size) return false;
                uint8_t b1 = src[i], b2 = src[i + 1];
                i += 2;
                int off = (((b1 & 0xF0) << 4) | b2) & 0xFFF;
                int length = (b1 & 0xF) + 2;
                if (off == 0 && length == 2) return true;
                int dist = pos - off;
                if (dist <= 0) dist += 0x1000;
                for (int k = 0; k < length; k++) {
                    out.push_back(size_t(dist) <= out.size() ? out[out.size() - size_t(dist)] : 0);
                }
                pos = (pos + length) & 0xFFF;
            }
        }
        return false;
    }
}

std::shared_ptr<const std::vector<uint8_t>> rush2::track1::main_code(const std::vector<uint8_t>& rom) {
    if (!main_data) {
        auto d = std::make_shared<std::vector<uint8_t>>();
        if (!lz(rom.data() + main_rom, rom.size() - main_rom, *d)) return nullptr;
        main_data = d;
    }
    return main_data;
}

bool rush2::track1::read_asset(const std::vector<uint8_t>& rom, int index, std::vector<uint8_t>& out) {
    auto m = main_code(rom);
    if (!m) return false;
    size_t o = asset_table - main_vram + size_t(index) * 4;
    uint32_t offset = (uint32_t((*m)[o]) << 24) | ((*m)[o + 1] << 16) | ((*m)[o + 2] << 8) | (*m)[o + 3];
    return lz(rom.data() + offset, rom.size() - offset, out);
}

static bool read_all(const fs::path& path, std::vector<uint8_t>& out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    out.assign(std::istreambuf_iterator<char>(f), {});
    return true;
}

static void write_all(const fs::path& path, const std::vector<uint8_t>& d) {
    std::ofstream f(path, std::ios::binary);
    f.write((const char*)d.data(), (std::streamsize)d.size());
}

int main(int argc, char** argv) {
    fs::path repo = argc > 1 ? argv[1] : ".";
    fs::path here = repo / "tools" / "rush1" / "cpp_test" / "out";
    const char* env = getenv("RUSH1_ROM");
    fs::path rom_path = env ? env : "E:\\Emulation\\roms\\n64\\San Francisco Rush - Extreme Racing.z64";
    std::vector<uint8_t> rom;
    if (!read_all(rom_path, rom)) {
        printf("can't read %s\n", rom_path.string().c_str());
        return 2;
    }
    int bad = 0;
    for (int t = 0; t < rush2::track1::track_count; t++) {
        rush2::track1::ConvertedTrack ct;
        std::string error;
        if (!rush2::track1::convert_track(rom, t, "HAWAII", ct, error)) {
            printf("track %d: %s\n", t + 1, error.c_str());
            bad++;
            continue;
        }
        struct File { const char* name; const std::vector<uint8_t>* data; };
        File files[] = { { "geometry", &ct.geometry }, { "placement", &ct.placement }, { "collision0", &ct.collision[0] },
                         { "collision1", &ct.collision[1] }, { "path0", &ct.path[0] }, { "path1", &ct.path[1] },
                         { "pvs", &ct.pvs } };
        for (const File& f : files) {
            std::vector<uint8_t> ref;
            fs::path rp = here / "ref" / (std::string(f.name) + std::to_string(t) + ".bin");
            if (!read_all(rp, ref)) {
                printf("track %d %s: no reference\n", t + 1, f.name);
                bad++;
                continue;
            }
            if (ref != *f.data) {
                size_t at = 0;
                while (at < ref.size() && at < f.data->size() && ref[at] == (*f.data)[at]) at++;
                printf("track %d %s: DIFFERS (C++ %zu bytes, Python %zu; first difference at 0x%zX)\n", t + 1, f.name,
                       f.data->size(), ref.size(), at);
                write_all(here / (std::string(f.name) + std::to_string(t) + ".bin"), *f.data);
                bad++;
            }
        }
        printf("track %d: lap %.1f / %.1f s, %d regions\n", t + 1, ct.lap_seconds[0], ct.lap_seconds[1], ct.pvs_count);
    }
    std::vector<uint8_t> asset3, out;
    if (read_all(here / "ref" / "asset3.bin", asset3)) {
        if (rush2::track1::extend_menu_container(asset3, rom, out)) {
            write_all(here / "asset3_rush1.bin", out);
            printf("asset 3: %zu -> %zu bytes; scales", asset3.size(), out.size());
            for (int t = 0; t < rush2::track1::track_count; t++) printf(" %.2f", rush2::track1::diorama_scale(rom, t));
            printf("\n");
        }
        else {
            bad++;
        }
    }
    printf(bad ? "%d PROBLEMS\n" : "ALL MATCH\n", bad);
    return bad ? 1 : 0;
}
