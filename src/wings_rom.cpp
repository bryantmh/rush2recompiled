// Reads files out of the Rush 2049 (USA) ROM.
//
// Rush 2049 keeps a table of ROM offsets for its 183 files in its main code; a file's data runs up to the next
// file's offset. Files are stored raw, raw-deflate compressed or LZ compressed. Only the LZ files are needed here.
//
// The LZ format is the 4KB-window LZSS Rush 2 also uses (func_80003C6C), except that Rush 2049's match distances are
// relative to the output position instead of absolute positions in a ring buffer. Each flag byte covers 8 items,
// least significant bit first: 1 is a literal byte, 0 is a two-byte match. A match's first byte holds the high nibble
// of the distance and the length - 2; its second byte holds the low byte of the distance. A match with distance 0 and
// length field 0 ends the data. Bytes before the start of the output read as zero.

#include <cstdio>

#include "wings_internal.h"

namespace {
    // ROM offsets of the Rush 2049 files used here (from the file table at 0x8010B5BC + 4 * index), with the offset of
    // the following file to give the compressed size.
    struct RomFile {
        uint32_t start;
        uint32_t end;
    };
    constexpr RomFile wing_model_file = { 0x3DE700, 0x3E2EE0 }; // File 77: WINGSWING1L..3R, WINGSFLAME1L..3R.
}

bool rush2::wings::lz_decompress(const uint8_t* src, size_t src_size, std::vector<uint8_t>& out) {
    out.clear();
    size_t pos = 0;
    while (pos < src_size) {
        uint8_t flags = src[pos++];
        for (int bit = 0; bit < 8; bit++) {
            if (flags & (1 << bit)) {
                if (pos >= src_size) {
                    return false;
                }
                out.push_back(src[pos++]);
                continue;
            }
            if (pos + 2 > src_size) {
                return false;
            }
            uint8_t b0 = src[pos++];
            uint8_t b1 = src[pos++];
            uint32_t distance = ((b0 & 0xF0) << 4) | b1;
            uint32_t length = (b0 & 0x0F) + 2;
            if (distance == 0 && length == 2) {
                return true;
            }
            for (uint32_t i = 0; i < length; i++) {
                out.push_back(distance <= out.size() ? out[out.size() - distance] : 0);
            }
        }
    }
    return false;
}

bool rush2::wings::read_wing_model_file(const std::vector<uint8_t>& rom, std::vector<uint8_t>& out) {
    if (rom.size() < wing_model_file.end) {
        return false;
    }
    if (!lz_decompress(rom.data() + wing_model_file.start, wing_model_file.end - wing_model_file.start, out)) {
        printf("[Wings] Failed to decompress the wing model\n");
        return false;
    }
    return true;
}
