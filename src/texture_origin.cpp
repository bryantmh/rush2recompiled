// Which game the data at an RDRAM address came from. See include/texture_origin.h.
//
// The game's textures all come from assets, which func_80077F20 / func_80077F84 decompress into the heap
// (src/assets.cpp). An asset the recomp replaces with converted Rush 2049 or SF Rush data is tagged with that game
// over the range it is copied to, and any other asset decompressed over a range takes the tag off. RT64 tells the
// texture observer (src/texture_upscale.cpp) the address each texture was loaded from, and Dump Textures sorts the
// texture by the tag found there.

#include <iterator>
#include <map>
#include <mutex>

#include "texture_origin.h"

namespace {
    constexpr uint32_t address_mask = 0x00FFFFFF;

    struct Range {
        uint32_t end;
        rush2::origin::Game game;
    };

    std::mutex mutex;
    std::map<uint32_t, Range> ranges; // by start, never overlapping

    // Takes [start, end) out of the ranges, keeping the parts of ranges around it (mutex held).
    void cut(uint32_t start, uint32_t end) {
        auto it = ranges.lower_bound(start);
        if (it != ranges.begin() && std::prev(it)->second.end > start) {
            --it;
        }
        while (it != ranges.end() && it->first < end) {
            uint32_t r_start = it->first;
            Range r = it->second;
            it = ranges.erase(it);
            if (r_start < start) {
                ranges[r_start] = { start, r.game };
            }
            if (r.end > end) {
                ranges[end] = { r.end, r.game };
                break;
            }
        }
    }
}

const char* rush2::origin::folder(Game game) {
    switch (game) {
        case Game::SFRush: return "sfrush";
        case Game::Rush2049: return "rush2049";
        case Game::Rush2049DC: return "rush2049dc";
        default: return "rush2";
    }
}

void rush2::origin::tag(uint32_t address, uint32_t size, Game game) {
    uint32_t start = address & address_mask, end = start + size;
    if (size == 0) {
        return;
    }
    std::lock_guard lock{ mutex };
    cut(start, end);
    if (game != Game::Rush2) {
        ranges[start] = { end, game };
    }
}

rush2::origin::Game rush2::origin::at(uint32_t address) {
    uint32_t a = address & address_mask;
    std::lock_guard lock{ mutex };
    auto it = ranges.upper_bound(a);
    if (it == ranges.begin()) {
        return Game::Rush2;
    }
    --it;
    return a < it->second.end ? it->second.game : Game::Rush2;
}
