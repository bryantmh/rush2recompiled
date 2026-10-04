// Reads any file out of the Rush 2049 (USA) ROM.
//
// Rush 2049 keeps a sorted table of ROM offsets for its 183 files at 0x8011B5BC in its main code segment, which is
// stored raw-deflate compressed at ROM 0xB0CB10 and loaded at 0x80086A50. A file's data runs up to the next file's
// offset; the main code follows the last file. Files are raw, raw-deflate compressed or LZ compressed
// (rush2::wings::lz_decompress); which one isn't recorded anywhere, so a file is taken as deflate if it inflates to
// the end of its data, else as LZ.

#include <cstdio>
#include <mutex>

#include "assets.h"
#include "rush2049_rom.h"
#include "wings.h"
#include "wings_internal.h"

namespace {
    constexpr uint32_t main_rom = 0xB0CB10;
    constexpr uint32_t main_vram = 0x80086A50;
    constexpr uint32_t file_table_vram = 0x8011B5BC;

    std::mutex table_mutex;
    const std::vector<uint8_t>* table_rom = nullptr; // The ROM the table was read from.
    std::vector<uint32_t> file_offsets; // file_count + 1 entries.

    uint32_t be32(const std::vector<uint8_t>& data, size_t offset) {
        return (data[offset] << 24) | (data[offset + 1] << 16) | (data[offset + 2] << 8) | data[offset + 3];
    }

    bool read_table(const std::vector<uint8_t>& rom) {
        if (table_rom == &rom && !file_offsets.empty()) {
            return true;
        }
        file_offsets.clear();
        table_rom = nullptr;
        std::vector<uint8_t> main_code;
        if (rom.size() <= main_rom || !rush2::assets::inflate_raw(rom.data() + main_rom, rom.size() - main_rom, main_code)) {
            return false;
        }
        size_t pos = file_table_vram - main_vram;
        while (pos + 4 <= main_code.size()) {
            uint32_t offset = be32(main_code, pos);
            if (!file_offsets.empty() && (offset <= file_offsets.back() || offset >= main_rom)) {
                break;
            }
            file_offsets.push_back(offset);
            pos += 4;
        }
        if (file_offsets.size() != rush2::rom2049::file_count) {
            printf("[2049] Unexpected file table (%zu files)\n", file_offsets.size());
            file_offsets.clear();
            return false;
        }
        file_offsets.push_back(main_rom);
        table_rom = &rom;
        return true;
    }
}

bool rush2::rom2049::read_file(const std::vector<uint8_t>& rom, int index, std::vector<uint8_t>& out) {
    uint32_t start, end;
    {
        std::lock_guard lock{ table_mutex };
        if (!read_table(rom) || index < 0 || index >= file_count) {
            return false;
        }
        start = file_offsets[index];
        end = file_offsets[index + 1];
    }

    // Inflating only succeeds on a complete deflate stream, which LZ data practically never forms. The sound banks
    // (6-8) and songs (10-21) are known LZ files, and inflating file 7 never returns, so those skip it.
    bool known_lz = (index >= 6 && index <= 8) || (index >= 10 && index <= 21);
    if (!known_lz && rush2::assets::inflate_raw(rom.data() + start, end - start, out) && !out.empty()) {
        return true;
    }
    if (rush2::wings::lz_decompress(rom.data() + start, end - start, out)) {
        return true;
    }
    printf("[2049] Failed to decompress file %d\n", index);
    return false;
}
