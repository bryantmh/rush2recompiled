#ifndef __RUSH2049_DC_INTERNAL_H__
#define __RUSH2049_DC_INTERNAL_H__

#include <cstdint>
#include <string>
#include <vector>

#include "rush2049_dc.h"

// Shared between src/rush2049_dc*.cpp.
namespace rush2::rom2049::dc {
    // Base address of the Dreamcast executable, 1ST_READ.BIN.
    constexpr uint32_t exe_base = 0x8C010000;

    // N64 file `index` made from the disc's files (src/rush2049_dc_convert.cpp). False if the disc has no counterpart
    // or it doesn't convert.
    bool convert_file(const Files& files, int index, std::vector<uint8_t>& out,
                      std::vector<SourceTexture>* shrunk = nullptr);

    // N64 name of a disc object (the two versions name some objects differently; src/rush2049_dc_model.cpp).
    std::string n64_name(const std::string& dc_name);

    // N64 model container `n64_file` (track geometry, objects, cars, HUD art...) made from the disc's container
    // `dc_file` (src/rush2049_dc_model.cpp). Objects become F3DEX2 lists in the N64's conventions; textures are
    // scaled to fit TMEM (the full-size ones are registered as RT64 replacements).
    // Textures it scaled down are added to shrunk.
    bool convert_model(const Files& files, const std::string& dc_file, int n64_file, std::vector<uint8_t>& out,
                       std::vector<SourceTexture>* shrunk = nullptr);
    // Texture record `index` of a decompressed disc container at full size (times tint), RGBA rows top down.
    bool decode_texture(const std::vector<uint8_t>& container, uint32_t index, uint32_t tint, std::vector<uint8_t>& rgba,
                        int& w, int& h);

    // N64 code segment s with the tables the recomp reads, from the disc's executable (src/rush2049_dc_tables.cpp).
    bool build_segment(const std::vector<uint8_t>& exe, Segment s, std::vector<uint8_t>& out);
    // True if exe is the executable the table map was made from (the USA disc).
    bool known_executable(const std::vector<uint8_t>& exe);

    // IMA ADPCM of the disc's music as the pack keeps it (src/rush2049_dc.cpp): 4 bits a sample, one 4-byte header
    // (s16 predictor, u8 step index, u8 0) per channel at the start of each block of block_samples samples.
    constexpr int music_block_samples = 8192;
    void adpcm_encode(const int16_t* pcm, int count, uint8_t* out);
    void adpcm_decode(const uint8_t* in, int count, int16_t* pcm);
}

#endif
