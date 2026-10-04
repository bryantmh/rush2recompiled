#ifndef __ASSETS_H__
#define __ASSETS_H__

#include <cstdint>
#include <vector>

// Replacement asset files (src/assets.cpp). An asset index is Rush 2's index into its asset table (0x800C185C);
// while replaced, the game's loaders get the given bytes instead of decompressing the asset from the ROM. Call these
// from game threads, before the game starts loading the asset.
namespace rush2::assets {
    void replace(uint8_t* rdram, int index, std::vector<uint8_t> data);
    void restore(uint8_t* rdram, int index);
    bool is_replaced(int index);
    // Decompresses the game's own copy of a deflate-compressed asset from the Rush 2 ROM, ignoring any replacement.
    bool read_original(uint8_t* rdram, int index, std::vector<uint8_t>& out);

    // Raw deflate (no zlib header), as used by both games.
    bool inflate_raw(const uint8_t* src, size_t src_size, std::vector<uint8_t>& out);
}

#endif
