#ifndef __RUSH2049_ROM_H__
#define __RUSH2049_ROM_H__

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

// Rush 2049 data (src/rush2049_rom.cpp for the N64 ROM, src/rush2049_dc.cpp for the Dreamcast disc).
//
// Everything that reads Rush 2049 goes through a Source, in the N64 ROM's terms: its 183 numbered files in their N64
// formats (big-endian, F3DEX2 models) and the tables of its code segments at their N64 addresses. A Dreamcast disc
// is converted to those on the fly, so the readers don't need to know which game they got.
namespace rush2::rom2049 {
    constexpr int file_count = 183;

    // N64 code segments the recomp reads tables from.
    enum class Segment {
        Boot,   // 0x80000400, raw at ROM 0x1000
        Main,   // 0x80086A50, raw deflate at ROM 0xB0CB10
        Battle, // 0x8038A400, the battle overlay, raw deflate at ROM 0xB6FEC4
    };
    constexpr uint32_t boot_vram = 0x80000400;
    constexpr uint32_t main_vram = 0x80086A50;
    constexpr uint32_t battle_vram = 0x8038A400;
    uint32_t segment_vram(Segment s);

    // A texture a Dreamcast source scaled down to fit TMEM: its N64 texels as the converted file loads them (RGBA16 or
    // RGBA32, big endian, rows top down) and where its full-size image is, so the renderer can draw that instead
    // (src/texture_upscale.cpp).
    struct SourceTexture {
        uint16_t w = 0, h = 0;
        bool rgba32 = false;
        std::vector<uint8_t> texels;
        std::string file;           // the disc's model container
        uint32_t index = 0;         // its texture record there
        uint32_t tint = 0xFFFFFF;   // RGB the image is multiplied by
    };

    class Source {
    public:
        virtual ~Source() = default;
        // Decompresses (or converts) file index into out in the N64 ROM's format. False if it can't be read.
        virtual bool read_file(int index, std::vector<uint8_t>& out) const = 0;
        // Code segment s, starting at segment_vram(s). Tables a Dreamcast disc has no counterpart for read as zero.
        // Null if it can't be read.
        virtual std::shared_ptr<const std::vector<uint8_t>> segment(Segment s) const = 0;
        // The big-endian N64 ROM, for the code that plays its audio data directly. Null for a Dreamcast disc.
        virtual const std::vector<uint8_t>* n64_rom() const { return nullptr; }
        virtual bool is_dreamcast() const { return false; }
        // Identifies this exact data for the converted-data disk cache (src/track_cache.cpp); 0 if it can't be cached.
        virtual uint64_t cache_key() const { return 0; }
        // A Dreamcast disc's file by name (upper case) as the pack keeps it: songs (.STR) as the pack's ADPCM, the rest
        // as stored on the disc. For the code that plays the disc's audio. False for the N64 ROM.
        virtual bool disc_file(const std::string& name, std::vector<uint8_t>& out) const { return false; }
        // The textures the files converted so far scaled down (a Dreamcast source), from the from'th on; count is how
        // many there are, which only grows.
        virtual size_t source_texture_count() const { return 0; }
        virtual std::vector<SourceTexture> source_textures(size_t from) const { return {}; }
        // A scaled-down texture's full-size image, RGBA rows top down.
        virtual bool source_image(const SourceTexture& t, std::vector<uint8_t>& rgba, int& w, int& h) const { return false; }
    };

    // A Source over the big-endian Rush 2049 (USA) N64 ROM.
    std::shared_ptr<const Source> n64_source(std::shared_ptr<const std::vector<uint8_t>> rom);

    // Decompresses file index of rom, the big-endian Rush 2049 ROM. Returns false if the ROM isn't readable or the
    // file doesn't decompress. Sources go through Source::read_file; this is the N64 one's.
    bool read_file(const std::vector<uint8_t>& rom, int index, std::vector<uint8_t>& out);

    // The texels of a texture record (TXHD, at file offset `record`) whose data is a texture-load list of RGBA16 or
    // RGBA32 texels, as a Dreamcast source's files have (the N64's named images are paletted texels instead): the
    // list's first G_SETTIMG, its render tile's format and its G_SETTILESIZE. Rows as stored. False otherwise.
    bool list_image(const std::vector<uint8_t>& file, uint32_t imag, uint32_t record, std::vector<std::array<uint8_t, 4>>& rgba,
                    int& w, int& h);
}

#endif
