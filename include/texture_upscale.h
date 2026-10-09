#ifndef __TEXTURE_UPSCALE_H__
#define __TEXTURE_UPSCALE_H__

// Texture upscaling (src/texture_upscale.cpp, src/texture_upscale_images.cpp, docs/texture_upscaling.md).

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "rush2049_rom.h"

namespace recomp::config {
    class Config;
}

namespace rush2::ui {
    class OptionsPage;
}

namespace rush2::upscale {
    // An 8-bit RGBA image, rows top to bottom.
    struct Image {
        uint32_t width = 0;
        uint32_t height = 0;
        std::vector<uint8_t> rgba;

        bool empty() const { return width == 0 || height == 0; }
    };

    // How a texture's edges continue when it's drawn: the tile's cms/cmt mode. Upscalers get the image padded this
    // way so their output has no seams where the texture repeats.
    enum class Edge : uint8_t { Wrap, Mirror, Clamp };

    // Pure image work, without the renderer or the UI (src/texture_upscale_images.cpp).

    // Decodes a texture from TMEM the way RT64's decoder does (sampleTMEM in TextureDecoder.hlsli). fmt, siz, tmem,
    // line and palette are the tile's fields (tmem and line in 64-bit words), tlut the G_TT_* mode. With a TLUT,
    // indices (when given) gets each texel's palette index.
    Image decode_tmem(const uint8_t* tmem_bytes, uint32_t fmt, uint32_t siz, uint32_t tmem, uint32_t line,
                      uint32_t palette, uint32_t tlut, uint32_t width, uint32_t height,
                      std::vector<uint8_t>* indices = nullptr);

    // Identifies a paletted texture by its indices alone: textures that differ only in their palette (car paint)
    // share it. Never 0.
    uint64_t index_key(const std::vector<uint8_t>& indices, uint32_t width, uint32_t height, uint32_t siz);

    // Identifies an image by its contents: textures RT64 hashes differently (another palette load, another tile)
    // but that decode to the same pixels share one upscale.
    uint64_t content_key(const Image& image);
    std::string key_name(uint64_t key); // 16 hex digits, the file name of the image everywhere.
    bool parse_key_name(const std::string& name, uint64_t& key); // From the first 16 characters of a file name.

    // Whether a texture is worth upscaling: big enough and not a single color.
    bool worth_upscaling(const Image& image);

    Image pad(const Image& image, uint32_t border, Edge edge_s, Edge edge_t);
    Image crop(const Image& image, uint32_t x, uint32_t y, uint32_t width, uint32_t height);
    Image resize_bilinear(const Image& image, uint32_t width, uint32_t height);

    // Fixes up an upscaler's output against the original: puts the original's alpha back (scaled up) when the
    // upscaler dropped it, and keeps cutout textures (alpha only 0 or 255) cutouts.
    void restore_alpha(Image& upscaled, const Image& original);

    // Undoes pad() on an upscaler's output: finds the scale from its size and cuts the border off. Returns an empty
    // image when the size isn't a whole multiple of the original's (with or without the border).
    Image unpad(const Image& output, const Image& original, uint32_t border);

    // Halves the image log2(factor) times with the alpha-weighted averaging of make_dds's mipmaps.
    Image downscale(const Image& image, uint32_t factor);

    // hq2x or hq4x (lib/hqx).
    Image hq_upscale(const Image& image, uint32_t scale);

    // Whether an upscaler's output, averaged back down, looks like the original. Catches garbage output.
    bool matches_original(const Image& upscaled, const Image& original);

    // Makes the upscale of a palette variant from the upscale of another variant of the same indices, without
    // upscaling again: old_colors is the texture the upscale was made from, new_colors the variant (same size).
    Image recolor(const Image& upscaled, const Image& old_colors, const Image& new_colors, Edge edge_s, Edge edge_t);

    // Level 0 of a DDS make_dds wrote.
    bool read_dds(const std::vector<uint8_t>& bytes, Image& image);

    // A DDS file (R8G8B8A8_UNORM) with the full mip chain, made with the same alpha-weighted averaging RT64 uses for
    // the mipmaps it generates, so upscaled textures blend down with distance like the originals.
    std::vector<uint8_t> make_dds(const Image& image);

    bool read_image(const std::filesystem::path& path, Image& image); // PNG, JPEG, TGA, BMP (anything stb_image reads).
    bool write_png(const std::filesystem::path& path, const Image& image);
    std::vector<uint8_t> read_file(const std::filesystem::path& path);
    bool write_file(const std::filesystem::path& path, const std::vector<uint8_t>& bytes);

    // Runs a command line and waits for it, without a console window and below normal priority. Returns its exit
    // code, or -1 when it couldn't be started or was stopped by setting cancel.
    int run_command(const std::string& command_line, const std::atomic<bool>* cancel = nullptr);

    // A path as UTF-8, for command lines.
    std::string path_utf8(const std::filesystem::path& path);

    // A texture pack: replacement images and the RT64 hashes each replaces.
    struct PackTexture {
        std::string path; // Relative path of the image in the pack, with its extension.
        std::vector<uint64_t> hashes;
    };
    // The pack's rt64.json.
    std::string pack_database(const std::vector<PackTexture>& textures);
    // The pack's mod.json.
    std::string pack_manifest(const std::string& id, const std::string& name, const std::string& description);
    // Zips a pack folder into an .rtz (mod.json and rt64.json at its root).
    bool zip_folder(const std::filesystem::path& folder, const std::filesystem::path& zip);

    // The game side (src/texture_upscale.cpp).
    void add_options(recomp::config::Config& config);
    void apply_loaded_options(recomp::config::Config& config);
    void add_buttons(rush2::ui::OptionsPage* page);
    // The Rush 2049 source (or null). A Dreamcast disc's textures, scaled down to fit TMEM, are drawn at their full
    // size whatever the upscaling mode: the scaled texture's content key finds the disc's image.
    void set_texture_source(std::shared_ptr<const rush2::rom2049::Source> source);
    extern const char* const mode_option_id;
    extern const char* const command_option_id;
}
    // Draws a CI8 texture with an exact full-color image instead of its 256 colors (the track banners): any CI8 tile
    // drawn in 2D whose palette indices are rows of indices (image.width x image.height, in the order the texture is
    // stored) is replaced by image, as the region of it those rows are, so a texture the game loads in strips is drawn
    // from the one image. Strips of one index (blank rows) are left alone. Works whatever the upscaling mode.
    void add_exact_image(std::vector<uint8_t> indices, Image image);

#endif
