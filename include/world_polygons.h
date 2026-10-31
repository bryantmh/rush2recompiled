#ifndef __WORLD_POLYGONS_H__
#define __WORLD_POLYGONS_H__

#include <cstdint>

// Rush 2's world polygons: 0x58 bytes each (+0 vertex count, 0 = free; +2 flags, the low bits the views it is drawn
// in; +4 texture; +6 primitive depth; then 0x14 per vertex: position, texture coordinates, color).
// polygon_create_80054010 takes the first free one, raising the count 0x800FAEF0 (and its high-water mark 0x800FAEF4)
// with no bound, and view_fog_setup_8007C624 draws them with its view's bit. The game kept 200 at 0x800FAF00, before
// the engine rpm (0x800FF3C0) and the music's loop start marker (0x800FF3C8): four views' sky domes (24 pieces each),
// the arrows over the other players' cars (src/arrows.cpp) and the game's own made 241, and the overflow wrote
// vertices over the loop marker, so the song's next loop sent the sequence player to a garbage address (a crash in
// al_seq_read_var_length on an online machine with four players). us.toml points the game's accesses at a copy below
// the 2D display lists (tools/players4/relocate.py, PLACED; the game heap's top is lowered for it, src/assets.cpp).
namespace rush2::world_polygons {
    constexpr uint32_t table = 0x80ACA000;
    constexpr uint32_t table_end = 0x80AE0000;
    constexpr uint32_t size = 0x58;
    constexpr int capacity = (int)((table_end - table) / size);   // 1,024
    constexpr uint32_t count = 0x800FAEF0;   // s32: slots in use
    constexpr uint32_t high = 0x800FAEF4;    // s32: the most slots used
}

#endif
