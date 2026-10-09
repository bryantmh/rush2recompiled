#ifndef __RUSH2049_DC_H__
#define __RUSH2049_DC_H__

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "rush2049_rom.h"
#include "texture_upscale.h"

// The Dreamcast Rush 2049 (USA) disc as a Rush 2049 source (src/rush2049dc/rush2049_dc*.cpp, docs/rush2049_research/dreamcast.md).
//
// Choosing a disc image copies the game's files out of it into the app folder (pack_file_name), so the image isn't
// needed afterwards. The pack holds the disc's files as they are; they're converted to the N64 ROM's files and
// tables when read (rush2::rom2049::Source).
namespace rush2::rom2049::dc {
    constexpr const char* pack_file_name = "rush2049_dc.pak";

    // True for the file types a Dreamcast disc image comes in (.cdi, .gdi, .iso).
    bool is_disc_image(const std::filesystem::path& path);

    enum class ImportResult { Good, FailedToOpen, NotADisc, WrongGame, WrongVersion, WriteFailed };
    // Reads Rush 2049 out of the disc image at `image` and writes the pack to `pack`. Takes a few seconds.
    ImportResult import(const std::filesystem::path& image, const std::filesystem::path& pack);
    // A Source over a pack written by import, or null if it isn't one this version reads.
    std::shared_ptr<const Source> open_pack(const std::filesystem::path& pack);

    // The RT64 replacement hash a scaled-down texture (Source::source_textures) is drawn with, for the texture pack of
    // the disc's full-size images (src/rush2049dc/rush2049_dc_pack.cpp). 0 for one that can't be hashed ahead (painted car
    // textures, whose palette the game sets).
    uint64_t replacement_hash(const SourceTexture& t);

    // The disc's textures at their full size (src/rush2049dc/rush2049_dc_pack.cpp), kept in
    // <app folder>/track_cache/dc_textures (built in the background once per disc and build). With a disc as the
    // source, its scaled-down textures are always drawn from those images: a texture pack RT64 loads like any other,
    // which installed packs override. With the N64 ROM as the source, a disc stored and the Dreamcast Textures option
    // (Settings > Graphics) on, an N64 texture whose picture is a disc texture's, scaled down, is drawn with the disc's
    // image instead (match, used by src/texture_upscale.cpp as live replacements). Images the player upscaled from the
    // dump (rush2049dc) take the disc's images' place in both.
    void set_textures_enabled(bool enabled);
    // The Rush 2049 source in use, and the stored disc pack (empty path: none) for N64 matching.
    void set_texture_sources(std::shared_ptr<const Source> active, const std::filesystem::path& disc_pack);

    enum class TextureMode { Off, Disc, N64 };
    TextureMode texture_mode();
    // Changes whenever what the disc's textures replace may have changed (mode, images, the player's upscales).
    uint64_t texture_generation();
    // Whether the texture pack in use (Disc mode) replaces the texture with this RT64 hash.
    bool in_texture_pack(uint64_t hash);

    // N64 mode: the disc image that is the same picture as an N64 texture (decoded, content key n64_key; intensity:
    // an I or IA texture, which only takes a gray image), at least its size. NotReady while the images are still
    // being built. Called off the display list thread: it can take a few milliseconds.
    enum class Match { NotReady, None, Found };
    Match match(const rush2::upscale::Image& n64, uint64_t n64_key, bool intensity, uint64_t& image_key);
    // Whether the player installed an upscale of the image (drawn as it is, never upscaled again).
    bool upscaled_by_player(uint64_t image_key);
    // A disc image's DDS (the player's upscale of it if they installed one) and a key naming its contents.
    bool image_file(uint64_t image_key, std::vector<uint8_t>& dds, uint64_t& content);
    // Just the key naming its contents (0: no such image), to skip reading a file RT64 has already.
    uint64_t image_content(uint64_t image_key);

    // Dump and install (Settings > Graphics > Your Own Upscales). dump_images writes the disc images in use as
    // <key>.png into dir, counting them in done: those of the textures drawn under the RT64 hashes given and the
    // images given (not the damaged car textures, which are made from the undamaged ones). 0 when no disc's images
    // are built.
    size_t dump_images(const std::filesystem::path& dir, const std::vector<uint64_t>& hashes,
                       const std::vector<uint64_t>& image_keys, std::atomic<size_t>* done);
    // Whether key names a disc image, and its full-size original.
    bool is_image(uint64_t key);
    bool original_image(uint64_t key, rush2::upscale::Image& image);
    // Replaces the player's installed upscales: begin, add each, then end (which puts them in use).
    void begin_upscales();
    bool add_upscale(uint64_t key, const rush2::upscale::Image& image);
    void end_upscales();

    // A disc's files by name (upper case, no version suffix), as the pack keeps them. Shared with the converters.
    class Files {
    public:
        virtual ~Files() = default;
        // The file as stored on the disc (.LZS still compressed). False if there is no such file.
        virtual bool raw(const std::string& name, std::vector<uint8_t>& out) const = 0;
        // The file, decompressed if it's an .LZS.
        bool get(const std::string& name, std::vector<uint8_t>& out) const;
    };
}

#endif
