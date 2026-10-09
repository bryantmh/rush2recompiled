// Rush 2049's LZ decompressor (its ROM files and the Dreamcast disc's .LZS files).
//
// The LZ format is the 4KB-window LZSS Rush 2 also uses (func_80003C6C), except that Rush 2049's match distances are
// relative to the output position instead of absolute positions in a ring buffer. Each flag byte covers 8 items,
// least significant bit first: 1 is a literal byte, 0 is a two-byte match. A match's first byte holds the high nibble
// of the distance and the length - 2; its second byte holds the low byte of the distance. A match with distance 0 and
// length field 0 ends the data. Bytes before the start of the output read as zero.

#include <cstdio>

#include "wings_internal.h"

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

// SHA-1, for identifying ROMs and disc files (the Rush 2049 and SF Rush pickers, src/rush2049dc/rush2049_dc_tables.cpp).
std::array<uint8_t, 20> rush2::wings::rom_sha1(const std::vector<uint8_t>& data) {
    uint32_t h[5] = { 0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0 };
    auto rol = [](uint32_t v, int n) { return (v << n) | (v >> (32 - n)); };

    std::vector<uint8_t> msg = data;
    uint64_t bit_len = uint64_t(data.size()) * 8;
    msg.push_back(0x80);
    while (msg.size() % 64 != 56) {
        msg.push_back(0);
    }
    for (int i = 7; i >= 0; i--) {
        msg.push_back(uint8_t(bit_len >> (i * 8)));
    }

    for (size_t chunk = 0; chunk < msg.size(); chunk += 64) {
        uint32_t w[80];
        for (int i = 0; i < 16; i++) {
            const uint8_t* p = &msg[chunk + i * 4];
            w[i] = (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
        }
        for (int i = 16; i < 80; i++) {
            w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
        }
        uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
        for (int i = 0; i < 80; i++) {
            uint32_t f, k;
            if (i < 20) { f = (b & c) | (~b & d); k = 0x5A827999; }
            else if (i < 40) { f = b ^ c ^ d; k = 0x6ED9EBA1; }
            else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDC; }
            else { f = b ^ c ^ d; k = 0xCA62C1D6; }
            uint32_t temp = rol(a, 5) + f + e + k + w[i];
            e = d; d = c; c = rol(b, 30); b = a; a = temp;
        }
        h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e;
    }

    std::array<uint8_t, 20> out;
    for (int i = 0; i < 5; i++) {
        out[i * 4 + 0] = uint8_t(h[i] >> 24);
        out[i * 4 + 1] = uint8_t(h[i] >> 16);
        out[i * 4 + 2] = uint8_t(h[i] >> 8);
        out[i * 4 + 3] = uint8_t(h[i]);
    }
    return out;
}
