#ifndef __TEXTURE_ORIGIN_H__
#define __TEXTURE_ORIGIN_H__

// Which game the data at an RDRAM address came from (src/texture_origin.cpp), so Dump Textures can sort the textures
// it writes into a folder per game.

#include <cstdint>

namespace rush2::origin {
    enum class Game : uint8_t { Rush2, SFRush, Rush2049, Rush2049DC };
    constexpr int game_count = 4;

    // The dump folder of a game's textures: rush2, sfrush, rush2049, rush2049dc.
    const char* folder(Game game);

    // [address, address + size) now holds data from game. Rush2 forgets what was there (Rush 2 is the default).
    void tag(uint32_t address, uint32_t size, Game game);
    // The game whose data is at address (any segment: the low 24 bits are what counts).
    Game at(uint32_t address);
}

#endif
