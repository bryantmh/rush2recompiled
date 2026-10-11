#ifndef __ASSETS_H__
#define __ASSETS_H__

#include <cstdint>
#include <vector>

#include "texture_origin.h"

// Replacement asset files (src/assets.cpp). An asset index is Rush 2's index into its asset table (0x800C185C);
// while replaced, the game's loaders get the given bytes instead of decompressing the asset from the ROM. Call these
// from game threads, before the game starts loading the asset.
namespace rush2::assets {
    // The part [offset, offset + size) of a replacement that came from game (rush2::origin, for Dump Textures).
    struct Span {
        uint32_t offset;
        uint32_t size;
        rush2::origin::Game game;
    };
    // spans: where the data came from, if not (all of it) from Rush 2.
    void replace(uint8_t* rdram, int index, std::vector<uint8_t> data, std::vector<Span> spans = {});
    // The same, all of it from game.
    void replace(uint8_t* rdram, int index, std::vector<uint8_t> data, rush2::origin::Game game);
    void restore(uint8_t* rdram, int index);
    bool is_replaced(int index);
    // Decompresses the game's own copy of a deflate-compressed asset from the Rush 2 ROM, ignoring any replacement.
    bool read_original(uint8_t* rdram, int index, std::vector<uint8_t>& out);
    // The asset as the game loads it now: a replacement's bytes and spans, else Rush 2's own. Callable from any thread
    // once the game has loaded an asset (it reads the asset table from that load's RDRAM). False if there is none.
    bool read_current(int index, std::vector<uint8_t>& out, std::vector<Span>& spans);
    constexpr int slot_count = 0x80; // Asset indices the moved tables have room for.
    // Counts changes to the replacements (a replace with new bytes, a restore): read_current's results are the same
    // while it is.
    uint64_t generation();
    // The game's RDRAM, once it has loaded an asset (for read_original off the game's threads), else null.
    uint8_t* rdram();

    // Raw deflate (no zlib header), as used by both games.
    bool inflate_raw(const uint8_t* src, size_t src_size, std::vector<uint8_t>& out);
}

#endif
