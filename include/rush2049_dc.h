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

    // True for the file types a Dreamcast disc image comes in (.cdi, .gdi, .cue, .iso, .chd).
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

    // Brake lights (issue #44). A Dreamcast car's tail lights are drawn in its body textures, which each paint job
    // has its own of: a texel red in every job's copy of a texture (the paint changes between jobs, the lights don't)
    // and next to another such texel is a light. jobs: each job's texels of one texture, w x h, RGBA5551 (big endian)
    // or RGBA32 (bytes R, G, B, A). One byte per texel, 1 for a light; empty when the texture has none.
    std::vector<uint8_t> brake_light_mask(const std::vector<const std::vector<uint8_t>*>& jobs, uint32_t w, uint32_t h,
                                          bool rgba32);
    // A texture with no paint jobs whose name says it's a tail light (C3_TAILIGHT): its red texels.
    std::vector<uint8_t> tail_texture_mask(const std::vector<uint8_t>& texels, uint32_t w, uint32_t h, bool rgba32);
    bool tail_texture_name(const std::string& name);
    // A texture of the car's front (C10_FRONT, C7_F, C8_LOWF, headlights): never lit as a brake light.
    bool front_texture_name(const std::string& name);
    // The texels with the mask's lit: bright red, as Rush 2 lights its own brake lights (0xF801, func_800599A0),
    // keeping a little of the lens's shading. A mask of another size (a scuffed or full-size copy) is scaled to it.
    std::vector<uint8_t> light_texels(const std::vector<uint8_t>& texels, uint32_t w, uint32_t h, bool rgba32,
                                      const std::vector<uint8_t>& mask, uint32_t mask_w, uint32_t mask_h);

    // The disc's textures at their full size (src/rush2049dc/rush2049_dc_pack.cpp), kept in
    // <app folder>/track_cache/dc_textures (built in the background once per disc and build). With a disc as the
    // source (Disc mode), or the N64 ROM as the source with a disc stored (N64 mode, since the track select offers the
    // race tracks' N64 and Dreamcast versions with either source), the disc's scaled-down textures are drawn from
    // those images, a texture pack RT64 loads like any other, which installed packs override. The N64 ROM's own
    // textures are left as they are. Images the player upscaled from the dump (rush2049dc) take the disc's images'
    // place.
    // The Rush 2049 source in use, and the stored disc pack (empty path: none).
    void set_texture_sources(std::shared_ptr<const Source> active, const std::filesystem::path& disc_pack);

    enum class TextureMode { Off, Disc, N64 };
    TextureMode texture_mode();
    // Changes whenever what the disc's textures replace may have changed (mode, images, the player's upscales).
    uint64_t texture_generation();
    // Whether the texture pack in use (Disc mode) replaces the texture with this RT64 hash.
    bool in_texture_pack(uint64_t hash);

    // Dump and install (Settings > Graphics > Your Own Upscales). dump_images writes every disc image as
    // <disc file>/<texture name>.png into dir (docs/asset_catalog.md), counting them in done, except the damaged car
    // textures, which are made from the undamaged ones, and, without interface, the interface files' (HUD, HUDBATTLE,
    // SELCAR, SELTRK). named gets each image's path (no extension) and key. 0 when no disc's images are built.
    size_t dump_images(const std::filesystem::path& dir, bool interface, std::atomic<size_t>* done,
                       std::vector<std::pair<std::string, uint64_t>>* named = nullptr);
    // Whether key names a disc image, and its full-size original.
    bool is_image(uint64_t key);
    // Whether the disc's images are built and loaded, so dump_images can answer.
    bool images_ready();
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
