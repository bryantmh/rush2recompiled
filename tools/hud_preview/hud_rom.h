// The race HUD's data read from the Rush 2 recomp ROM (rush2.us.recomp.z64, made by tools/extract.py): the HUD's
// layout tables and its images, decoded as the game draws them (src/asset_catalog_walk.cpp). For tools/hud_preview.
#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace hud_rom {
    struct Entry {
        std::string name;   // "" none, "-1" a callback widget
        int16_t x = 0, y = 0;
        uint32_t callback = 0, param = 0;
    };
    struct Image {
        int w = 0, h = 0;
        std::vector<uint8_t> rgba;  // top row first
    };
    struct Rom {
        std::vector<uint8_t> data;
        // The HUD's images by name, for language 0 (English), 1 (French), 2 (German).
        std::map<std::string, Image> images[3];
        // Rush 2049's that the recomp draws in the HUD (ODOMETER, HEALTHBG), from its ROM's HUD file; empty without it.
        std::map<std::string, Image> images2049;
        bool load(const std::string& path, std::string& error);
        // Reads Rush 2049's images from its ROM (build/rush2049.z64, the game's copy). False if it can't.
        bool load2049(const std::string& path);
        uint32_t word(uint32_t vram) const;
        int16_t half(uint32_t vram) const;
        std::string string(uint32_t vram) const;
        std::vector<Entry> table(uint32_t vram, int count) const;
        const Image* image(const std::string& name, int language) const;
    };
}
