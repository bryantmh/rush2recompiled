#pragma once
// A CD or GD-ROM disc image compressed as CHD (MAME's "Compressed Hunks of Data", what chdman makes from a .cue or
// .gdi), read with libchdr (lib/libchdr). Shared by the Dreamcast Rush 2049 reader (src/rush2049dc/rush2049_dc.cpp)
// and the PlayStation SF Rush reader (src/sfrush_ps1/ps1_disc.cpp). See src/chd_image.cpp for the layout.

#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace rush2::chd {
    // A frame as a .bin stores it: 2352 bytes (the CHD's 96 bytes of subchannel after each frame are left out).
    constexpr uint32_t frame_size = 2352;

    struct Track {
        int number = 0;
        std::string type;          // "MODE1", "MODE1_RAW", "MODE2_RAW", "MODE2_FORM_MIX", "AUDIO", ...
        bool audio = false;
        uint32_t header = 0;       // Bytes before a frame's 2048 bytes of data (data tracks).
        uint32_t chd_frame = 0;    // The CHD frame holding the track's INDEX 01 (its pregap, when stored, is before it).
        uint32_t frames = 0;       // Frames from INDEX 01 to the track's end.
        uint32_t lba = 0;          // The disc's sector number of INDEX 01 (45000 for a GD-ROM's track 3).
    };

    class Image {
    public:
        Image();
        ~Image();
        Image(const Image&) = delete;
        Image& operator=(const Image&) = delete;
        // False if `path` isn't a CD or GD-ROM CHD that libchdr can read.
        bool open(const std::filesystem::path& path);
        const std::vector<Track>& tracks() const { return list; }
        bool gdrom() const { return is_gdrom; }
        // Copies CHD frame `frame`'s 2352 bytes to out. Audio frames come out little-endian, as a .bin holds them.
        // Safe from several threads at once.
        bool read_frame(uint32_t frame, uint8_t* out) const;

    private:
        struct State;
        std::unique_ptr<State> state;
        std::vector<Track> list;
        bool is_gdrom = false;
    };
}
