#ifndef __RUSH2049_DC_H__
#define __RUSH2049_DC_H__

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "rush2049_rom.h"

// The Dreamcast Rush 2049 (USA) disc as a Rush 2049 source (src/rush2049_dc*.cpp, docs/rush2049_research/dreamcast.md).
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
