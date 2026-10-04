#ifndef __RUSH2049_ROM_H__
#define __RUSH2049_ROM_H__

#include <cstdint>
#include <vector>

// Rush 2049 (USA) ROM files (src/rush2049_rom.cpp).
namespace rush2::rom2049 {
    constexpr int file_count = 183;

    // Decompresses file index of rom, the big-endian Rush 2049 ROM (rush2::wings::get_rom()). Returns false if the ROM
    // isn't readable or the file doesn't decompress.
    bool read_file(const std::vector<uint8_t>& rom, int index, std::vector<uint8_t>& out);
}

#endif
