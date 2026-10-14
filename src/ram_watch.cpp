// Test aid for working out game variables (docs/online.md#test-aids), run once per game frame from
// src/online/online_game.cpp:
//
//   RUSH2_RAM_WATCH=ADDR:SIZE[,ADDR:SIZE...]   prints "[ram] frame N ADDR: old -> new" whenever a value changes (SIZE 1, 2
//                                              or 4; hex addresses, e.g. 8010C0D0:4)
//   RUSH2_RAM_DUMP=DIR                         writes RDRAM 0x80000000-0x80800000 to DIR/ram_<frame>_<ADDR>_<value>.bin
//                                              each time a watched value changes
//
// Snapshots taken around a change (a menu starting a race, say) can then be diffed outside the game.

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "recomp.h"

namespace rush2::ram_watch {
    void frame(uint8_t* rdram, uint32_t frame_number);
}

namespace {
    struct Watch {
        uint32_t address;
        int size;
        uint32_t value;
        bool seen;
    };
    std::vector<Watch> watches;
    std::string dump_dir;
    bool parsed = false;

    void parse() {
        parsed = true;
        const char* spec = std::getenv("RUSH2_RAM_WATCH");
        if (spec == nullptr) {
            return;
        }
        std::string text = spec;
        size_t pos = 0;
        while (pos < text.size()) {
            size_t end = text.find(',', pos);
            std::string entry = text.substr(pos, end == std::string::npos ? std::string::npos : end - pos);
            pos = end == std::string::npos ? text.size() : end + 1;
            size_t colon = entry.find(':');
            Watch w{};
            w.address = (uint32_t)std::strtoul(entry.substr(0, colon).c_str(), nullptr, 16);
            w.size = colon == std::string::npos ? 4 : std::atoi(entry.c_str() + colon + 1);
            if (w.size == 1 || w.size == 2 || w.size == 4) {
                watches.push_back(w);
            }
        }
        if (const char* dir = std::getenv("RUSH2_RAM_DUMP")) {
            dump_dir = dir;
            std::error_code ec;
            std::filesystem::create_directories(dump_dir, ec);
        }
    }

    uint32_t read(uint8_t* rdram, const Watch& w) {
        switch (w.size) {
            case 1: return MEM_BU(0, (int32_t)w.address);
            case 2: return MEM_HU(0, (int32_t)w.address);
            default: return (uint32_t)MEM_W(0, (int32_t)w.address);
        }
    }

    void dump(uint8_t* rdram, uint32_t frame_number, const Watch& w, uint32_t value) {
        char name[96];
        std::snprintf(name, sizeof(name), "ram_%06u_%08X_%X.bin", frame_number, w.address, value);
        std::ofstream out{ std::filesystem::path(dump_dir) / name, std::ios::binary };
        // RDRAM is byte-swapped within words in the recomp; write it in N64 (big-endian) byte order.
        std::vector<uint8_t> bytes(0x800000);
        for (uint32_t i = 0; i < bytes.size(); i++) {
            bytes[i] = (uint8_t)MEM_BU(0, (int32_t)(0x80000000u + i));
        }
        out.write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    }
}

void rush2::ram_watch::frame(uint8_t* rdram, uint32_t frame_number) {
    if (!parsed) {
        parse();
    }
    for (Watch& w : watches) {
        uint32_t value = read(rdram, w);
        if (!w.seen || value != w.value) {
            std::fprintf(stderr, "[ram] frame %u %08X: %X -> %X\n", frame_number, w.address, w.seen ? w.value : 0, value);
            if (w.seen && !dump_dir.empty()) {
                dump(rdram, frame_number, w, value);
            }
            w.value = value;
            w.seen = true;
        }
    }
}
