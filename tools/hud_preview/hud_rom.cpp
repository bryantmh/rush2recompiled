// The race HUD's data read from the Rush 2 recomp ROM (hud_rom.h).
#define STB_IMAGE_IMPLEMENTATION // texture_upscale_images.cpp decodes PNGs (RT64 has it in the game)
#include "stb/stb_image.h"

#include "hud_rom.h"

#include <fstream>

#include "asset_catalog.h"
#include "rush2049_rom.h"
#include "track2049_convert.h"

namespace {
    constexpr uint32_t main_rom = 0x01000000;   // the main segment, uncompressed (tools/extract.py)
    constexpr uint32_t main_vram = 0x800539E0;
    constexpr uint32_t main_size = 0x7C730;
    constexpr uint32_t asset_table = 0x800C185C; // ROM offsets of the assets (u32 each)

    // The containers the race HUD's images come from: the race's shared one (the digits, the time font, the track
    // map's background), the HUD's own and the language's (16 English, 17 French, 18 German).
    constexpr int race_asset = 0x12, hud_asset = 0x13, language_asset = 0x16;
}

namespace hud_rom {
    uint32_t Rom::word(uint32_t vram) const {
        uint32_t o = main_rom + (vram - main_vram);
        if (vram < main_vram || vram + 4 > main_vram + main_size || o + 4 > data.size()) return 0;
        return (uint32_t(data[o]) << 24) | (uint32_t(data[o + 1]) << 16) | (uint32_t(data[o + 2]) << 8) | data[o + 3];
    }

    int16_t Rom::half(uint32_t vram) const {
        uint32_t o = main_rom + (vram - main_vram);
        if (vram < main_vram || vram + 2 > main_vram + main_size || o + 2 > data.size()) return 0;
        return (int16_t)((data[o] << 8) | data[o + 1]);
    }

    std::string Rom::string(uint32_t vram) const {
        std::string s;
        uint32_t o = main_rom + (vram - main_vram);
        if (vram < main_vram || vram >= main_vram + main_size) return s;
        while (o < data.size() && data[o] != 0 && s.size() < 32) s += (char)data[o++];
        return s;
    }

    std::vector<Entry> Rom::table(uint32_t vram, int count) const {
        std::vector<Entry> v;
        for (int i = 0; i < count; i++) {
            uint32_t r = vram + (uint32_t)i * 0x28;
            Entry e;
            uint32_t name = word(r);
            e.name = name == 0 ? "" : name == 0xFFFFFFFFu ? "-1" : string(name);
            e.x = half(r + 4);
            e.y = half(r + 6);
            e.callback = word(r + 0x20);
            e.param = word(r + 0x24);
            v.push_back(e);
        }
        return v;
    }

    const Image* Rom::image(const std::string& name, int language) const {
        auto it = images[language].find(name);
        if (it != images[language].end()) return &it->second;
        it = images[0].find(name);
        return it != images[0].end() ? &it->second : nullptr;
    }

    bool Rom::load2049(const std::string& path) {
        std::ifstream f(path, std::ios::binary);
        std::vector<uint8_t> rom((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        if (rom.empty()) return false;
        // The odometer's digits are in the HUD file (src/rush2049/odometer.cpp), the battle bar's frame in the battle
        // HUD's (63, track2049_convert.cpp battle_hud_file).
        struct Want { int file; const char* name; };
        for (const Want& want : { Want{ rush2::rom2049::hud_file, "ODOMETER" }, Want{ 63, "HEALTHBG" } }) {
            std::vector<uint8_t> file;
            std::vector<std::array<uint8_t, 4>> rgba;
            int w = 0, h = 0;
            const char* name = want.name;
            if (!rush2::rom2049::read_file(rom, want.file, file) || !rush2::rom2049::file_image(file, name, rgba, w, h)) continue;
            Image& im = images2049[name];
            im.w = w;
            im.h = h;
            im.rgba.resize(rgba.size() * 4);
            for (size_t i = 0; i < rgba.size(); i++) {
                for (int k = 0; k < 4; k++) im.rgba[i * 4 + k] = rgba[i][k];
            }
        }
        return !images2049.empty();
    }

    bool Rom::load(const std::string& path, std::string& error) {
        std::ifstream f(path, std::ios::binary);
        data.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
        if (data.size() < main_rom + main_size) {
            error = "can't read the Rush 2 recomp ROM " + path + " (tools/extract.py makes it)";
            return false;
        }
        auto asset = [&](int i, std::vector<uint8_t>& out) {
            uint32_t o = word(asset_table + (uint32_t)i * 4);
            if (o >= data.size()) return false;
            return rush2::track2049::rush2_lz_decompress(data.data() + o, data.size() - o, out);
        };
        auto add = [&](int a, int language) {
            std::vector<uint8_t> file;
            rush2::catalog::Container c;
            std::string err;
            if (!asset(a, file) || !rush2::catalog::read_container(file, c, err, false)) {
                error = "can't read asset " + std::to_string(a) + ": " + err;
                return false;
            }
            for (const rush2::catalog::Texture& t : c.textures) {
                if (t.name.find('.') != std::string::npos || t.name.find('~') != std::string::npos) continue;
                Image im;
                im.w = (int)t.width;
                im.h = (int)t.height;
                im.rgba.resize((size_t)im.w * im.h * 4);
                // Format 5 (the big digits) is an intensity the game tints with the primitive color, texels of 0
                // transparent; the catalog doesn't decode it, so it's read from the texels here.
                std::vector<uint8_t> intensity;
                if (t.fmt == 5) {
                    size_t n = (size_t)im.w * im.h;
                    size_t bytes = t.siz == 0 ? (n + 1) / 2 : n;
                    if (t.texels + bytes > file.size()) continue;
                    intensity.resize(n * 4);
                    for (size_t i = 0; i < n; i++) {
                        uint8_t b = file[t.texels + (t.siz == 0 ? i / 2 : i)];
                        uint8_t v = t.siz == 0 ? (uint8_t)(((i & 1) ? b & 0xF : b >> 4) * 17) : b;
                        uint8_t* o = &intensity[i * 4];
                        o[0] = o[1] = o[2] = v;
                        o[3] = v ? 255 : 0;
                    }
                }
                const std::vector<uint8_t>& src = t.fmt == 5 ? intensity : t.image.rgba;
                // The catalog's rows are as stored, bottom row first (docs/hud_layout.md#images).
                for (int y = 0; y < im.h; y++) {
                    for (int x = 0; x < im.w; x++) {
                        const uint8_t* s = &src[((size_t)(im.h - 1 - y) * im.w + x) * 4];
                        std::copy(s, s + 4, &im.rgba[((size_t)y * im.w + x) * 4]);
                    }
                }
                images[language][t.name] = std::move(im);
            }
            return true;
        };
        if (!add(race_asset, 0) || !add(hud_asset, 0)) return false;
        for (int l = 0; l < 3; l++) {
            if (!add(language_asset + l, l)) return false;
        }
        return true;
    }
}
