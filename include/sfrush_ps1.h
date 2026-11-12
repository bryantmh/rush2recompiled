#ifndef __SFRUSH_PS1_H__
#define __SFRUSH_PS1_H__

#include <array>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

namespace rush2::ui {
    class OptionsPage;
}
namespace rush2::chd {
    class Image;
}

// The PlayStation San Francisco Rush's own content (src/sfrush_ps1/, docs/sfrush_ps1.md): the track and cars the N64
// game doesn't have, built from the PS1 disc into one mod. The N64 ROM's content is never replaced: the disc adds only
// what is its own, with or without the N64 ROM.
//   - Track 4 (as the custom track "chinatown", listed after the SF Rush tracks)
//   - Seven cars: STARSKY, JEEP, JETCAR, MINI, BBUS (the city bus), PIGGY (the police car) and the UFO
// The Games tab picks the disc (.cue, .ccd, .chd, .iso or the data track's .bin/.img) and builds and installs the mod; a new build or another
// disc rebuilds it once at the next launch, as the arcade mods are (src/arcade2049/arcade_install.cpp).
namespace rush2::sfrush_ps1 {
    inline constexpr const char* mod_id = "sfrush_ps1";

    // The disc's files by their path (upper case, '/' separated), read from the data track (MODE2/2352, ISO 9660).
    class Disc {
    public:
        // `path`: the .cue (its first FILE is the data track), a CloneCD .ccd (its .img), a .chd (its first track), or
        // the data track itself (.bin/.img raw 2352-byte sectors, .iso 2048, or 2336).
        bool open(const std::filesystem::path& path, std::string& error);
        bool read(const std::string& name, std::vector<uint8_t>& out) const;
        // A .DAT file split into the chunks its .SIZ lists (s32 sizes). `base` is the path without the extension.
        bool chunks(const std::string& base, std::vector<std::vector<uint8_t>>& out) const;
        bool has(const std::string& name) const { return files.count(name) != 0; }
        const std::filesystem::path& data_track() const { return bin; }

    private:
        std::filesystem::path bin;                    // the data track's file (or the .chd)
        uint32_t sector_size = 2352, data_offset = 24;
        std::shared_ptr<const rush2::chd::Image> chd;  // a .chd's image; its first track starts at CHD frame chd_frame
        uint32_t chd_frame = 0;
        std::map<std::string, std::pair<uint32_t, uint32_t>> files;   // name -> (first sector, size)
        bool read_sectors(uint32_t lba, uint32_t size, std::vector<uint8_t>& out) const;
        bool walk(uint32_t lba, uint32_t size, const std::string& path, int depth);
    };

    // EXE/RUSH.EXE as the console loads it (a PS-X EXE at 0x80010000), for the tables and code the converters read.
    struct Exe {
        static constexpr uint32_t base = 0x80010000;
        std::vector<uint8_t> data;           // from `base`
        std::set<uint32_t> jal_targets;      // every jal's target: where functions start
        bool open(const Disc& disc);
        bool in(uint32_t a, size_t n) const { return a >= base && a - base + n <= data.size(); }
        uint32_t w(uint32_t a) const {
            const uint8_t* p = &data[a - base];
            return p[0] | p[1] << 8 | p[2] << 16 | uint32_t(p[3]) << 24;
        }
        int8_t b(uint32_t a) const { return int8_t(data[a - base]); }
        // The next function's start after `a` (or the end of the program).
        uint32_t func_end(uint32_t a) const;
    };

    // A PSV model (cars, the wheel, the knock-over props; docs/sfrush_ps1.md#cars): +0x38 u32 vertex count, face
    // count, vertex offset, face offset; vertices 3 x s32 16.16 (the loader scales them by a1 / 64 into 0.1 ft); faces
    // u32 flags (bits 24-31: the CLUT, an index of 0x800A7B08, for track props), u32 n (3 or 4), u32 index[n],
    // (u32 u, u32 v) << 16 x n, u32 texture, u32[n].
    struct PsvFace {
        uint32_t flags = 0;
        std::vector<uint32_t> idx;
        std::vector<std::array<double, 2>> uv;   // texels (u, v)
        uint32_t tex = 0;
    };
    bool read_psv(const std::vector<uint8_t>& data, std::vector<std::array<double, 3>>& verts, std::vector<PsvFace>& faces);

    // A face corner's texture coordinate (texels) as Rush 2 should sample it. The PS1 point-samples: a face whose
    // coordinates span 0 to 4 shows texels 0 to 3, each whole. Rush 2 filters, so a corner on a texel boundary blends in
    // the texel past it, and a narrow strip stretched over a big face (the City Bus's roof: v 0 to 4 of the bus texture,
    // solid paint, with grey rows 5 and 6 under it) came out in streaks of the next rows. Each corner moves half a
    // texel toward the face's middle (`centre`, its corners' mean) in u and in v, at most halfway there, so the
    // corners sample the PS1's outermost texels at their centers (docs/sfrush_ps1.md#texture-coordinates).
    inline std::array<double, 2> texel_inset(const std::array<double, 2>& uv, const std::array<double, 2>& centre) {
        std::array<double, 2> out = uv;
        for (int i = 0; i < 2; i++) {
            double d = centre[i] - uv[i];
            double step = d > 0 ? 0.5 : -0.5;
            out[i] += std::abs(d) < 1.0 ? d / 2.0 : step;
        }
        return out;
    }

    // Whether `path` is (or is the .cue of) the supported disc: SLUS-00505 with the files the mod is built from.
    bool is_disc(const std::filesystem::path& path);

    // The disc's CD audio tracks (its music: four songs on the supported disc), from its .cue (or the .cue next to
    // the data track's .bin), its .ccd (or the .ccd next to the .img) or its .chd: each track's file, where its 16-bit
    // stereo 44.1 kHz frames are in it (byte offset and length; in a .chd, `offset` is the CHD frame), and its number.
    // An .iso alone has no music.
    struct AudioTrack {
        std::filesystem::path file;
        uint64_t offset = 0, length = 0;
        int number = 0;
        bool chd = false;
    };
    std::vector<AudioTrack> audio_tracks(const std::filesystem::path& disc);
    // A track's samples (little-endian PCM16 stereo, as a .bin holds them). Slow for a .chd: call from a worker.
    bool read_audio(const AudioTrack& track, std::vector<uint8_t>& out);

    // The mod's files by their path in the mod. The log gets what each conversion did.
    using ModFiles = std::map<std::string, std::vector<uint8_t>>;
    // The N64 SF Rush's forward AI lane files for its tracks 0-2 (Golden Gate, Embarcadero, Market: ROM assets 58-60,
    // src/rush1/rush1_rom.cpp read_asset), when the user has the N64 ROM. The PS1's versions of those courses then
    // take the N64's lines (ps1_track.cpp, docs/sfrush_ps1.md#ai-lanes); an empty one keeps the PS1's own.
    using N64Lanes = std::array<std::vector<uint8_t>, 3>;
    // The N64 SF Rush's Embarcadero as placed (track1::convert_track, then catalog::read_container and
    // for_each_placed_triangle: the game's world, feet), when the user has the N64 ROM. The PS1's Embarcadero takes its
    // waterfront freeway overpass from it (ps1_track.cpp, docs/sfrush_ps1.md#embarcadero-overpass); empty keeps the
    // PS1's own, patched.
    struct N64Scene {
        struct Image {
            uint32_t width = 0, height = 0;
            std::vector<uint8_t> rgba;
            int wrap_s = 10497, wrap_t = 10497;     // glTF sampler wrap modes (repeat, mirrored 33648, clamp 33071)
        };
        struct Tri {
            float p[3][3];
            float uv[3][2];                         // in units of the image's size
            uint8_t color[3][4];
            int image = -1;
        };
        std::vector<Image> images;
        std::vector<Tri> tris;
    };
    bool build_mod(const Disc& disc, const std::filesystem::path& assets_dir, ModFiles& out, std::string& log,
                   const N64Lanes& n64 = {}, const N64Scene& n64_embarcadero = N64Scene{});
    bool build_track(const Disc& disc, const std::filesystem::path& assets_dir, ModFiles& out, std::string& log,
                     const N64Lanes& n64 = {}, const N64Scene& n64_embarcadero = N64Scene{});
    bool build_cars(const Disc& disc, const std::filesystem::path& assets_dir, ModFiles& out, std::string& log);
    // The track select diorama (src/sfrush_ps1/ps1_diorama.cpp) as a glb: the hand-made layout `spec` (JSON) round
    // the route (the AI path's middle, a closed loop, glTF meters as track.glb has it). look: the SNOW / NIGHT
    // version's (rush2::diorama_look::Look as int: 0 day, 1 snow, 2 night).
    bool build_diorama(const std::vector<std::array<double, 3>>& route, const std::filesystem::path& spec,
                       std::vector<uint8_t>& glb, std::string& log, int look = 0);

    // The game (src/sfrush_ps1/ps1_install.cpp).
    void add_games_section(rush2::ui::OptionsPage* page, std::function<void()>& refresh);
    // The launch's build pass (src/prewarm.cpp): an installed mod built by another build or from another disc is built
    // again now, on the calling thread.
    void refresh_now();
    bool rebuilt();
    // The disc the mod is built from (the one picked this session, else the installed mod's), or empty when it isn't
    // where it was. Any thread.
    std::filesystem::path disc();
}

#endif
