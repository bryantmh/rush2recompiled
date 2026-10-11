#ifndef __TEXTURE_CENSUS_H__
#define __TEXTURE_CENSUS_H__

// Test aid (src/texture_census.cpp, docs/asset_catalog.md "Texture census"): with RUSH2_TEXTURE_CENSUS=<file>, every
// 3D texture drawn is recorded with the RDRAM bytes its texels were loaded from, and RUSH2_TEXTURE_DUMP's timer
// writes <file>: each drawn texture the catalog has no image of (by content key or palette indices), with where its
// loaded bytes sit in the containers the catalog reads, so it can be traced to its ROM data.

#include <cstdint>

#include "texture_upscale.h"

namespace rush2::census {
    bool enabled();
    // The game's RDRAM (main.cpp, when the renderer is created).
    void set_rdram(const uint8_t* rdram);
    // A 3D texture drawn for the first time under hash (from the upscaler's observer).
    void record(uint64_t hash, uint64_t key, uint64_t index_key, uint32_t address, uint8_t fmt, uint8_t siz,
                uint16_t line, uint16_t tmem, uint16_t width, uint16_t height, uint32_t tlut,
                const rush2::upscale::Image& image);
    // Compares what was drawn with the catalog and writes the report (a few seconds, on the calling thread).
    void write();
}

#endif
