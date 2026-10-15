#ifndef __TRACK_MOD_H__
#define __TRACK_MOD_H__

#include <array>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "track2049_convert.h"

// Custom tracks from mods (docs/custom_tracks.md): a mod's tracks/<id>/ folder (or the older rush2/tracks/<id>/) holds track.glb (the track built
// in Blender) and track.toml (its settings). The converter (src/track_mod/track_mod_convert.cpp) turns them into the
// files of a Rush 2 track slot, the same set a converted Rush 2049 track has, and they are raced in a borrowed slot the
// way the 2049 tracks are (src/rush2049/track2049.cpp).
namespace rush2::track_mod {
    struct Message {
        enum Level { error, warning, info } level;
        std::string text;
    };

    // What the converter found wrong or worth knowing, written for the track's author.
    struct Report {
        std::vector<Message> messages;
        void error(const std::string& t) { messages.push_back({ Message::error, t }); }
        void warning(const std::string& t) { messages.push_back({ Message::warning, t }); }
        void info(const std::string& t) { messages.push_back({ Message::info, t }); }
        bool ok() const {
            for (const Message& m : messages) if (m.level == Message::error) return false;
            return true;
        }
        std::string text() const;
    };

    // A [surfaces.<name>] entry: a custom collision surface (COL_<NAME>_*).
    struct Surface {
        std::string like;   // "road", "dirt" or "grass": whose effects and sounds it uses
        float drag = 0.0f;  // the dirt/grass drag coefficient (stock dirt 10, grass 15)
        int material = 0;   // the polygon material slot it was given (3-15, not 8)
    };

    // track.toml.
    struct Settings {
        std::string name, author, version, description;
        std::optional<float> start_time;
        std::vector<std::array<float, 3>> checkpoint_time;   // per checkpoint: first lap, later laps (third unused)
        std::vector<float> best_times;
        std::array<uint8_t, 3> fog = { 0xA0, 0xB8, 0xD0 };
        std::string sky = "none";                             // not supported yet
        float cloud_scroll = 0.0f, sky_parallax = 0.0f;
        std::string music;
        std::string preview, logo, diorama;                   // [menu] files in the track's folder
        std::map<std::string, Surface> surfaces;              // by upper-case name
    };

    bool parse_settings(const std::string& toml_text, Settings& out, Report& report);

    // A true-color stand-in texture of the converted geometry: RGBA16 texels at `offset` in the geometry, drawn from
    // image `image` of the glb. On screen the stand-in is replaced by the full-size image (tinted and cut out as the
    // texels are) through rush2::derive::set_mod_textures, keyed by `key` (src/track_mod/track_mod.cpp).
    struct Texture {
        enum Source : uint32_t { track = 0, diorama = 1 };
        uint32_t source = track;                        // which glb: track.glb or the [menu] diorama
        int image = -1;
        uint32_t width = 0, height = 0;
        uint32_t offset = 0;
        std::vector<uint8_t> texels;
        std::array<float, 4> tint = { 1, 1, 1, 1 };    // the material's baseColorFactor
        float cutoff = 0.0f;                            // alpha below cutoff x 255 is clear (0: opaque)
        uint64_t key = 0;                               // rush2::upscale::rgba16_key of the texels
    };

    // A custom track's track select art, made with the track and appended to the track select's models
    // (src/track_mod/track_mod_menu.cpp) as MODTRKnn / MODLOGOnn.
    struct MenuArt {
        std::vector<uint8_t> logo;              // 128 x 32 CI8 texels, rows bottom-up as Rush 2 stores its logos
        std::vector<uint8_t> logo_palette;      // 256 RGBA5551 colors, big endian
        std::vector<uint8_t> logo_rgba;         // the logo in full color, 128 x 32 RGBA in the texels' row order
        std::vector<uint8_t> diorama;           // the diorama model's texels, vertices and list, offsets from its start
        uint32_t diorama_list = 0;              // the list's offset
        std::vector<uint32_t> diorama_pointers; // offsets of the words in it holding offsets into it (G_VTX, G_SETTIMG)
        float diorama_radius = 0.0f;            // largest vertex distance from its origin, world units (model units / 16)
        bool route_preview = false;             // no authored diorama: Rush 2049's track select preview of the route
    };

    // The optional files besides track.glb and track.toml that the conversion reads ([menu] in track.toml).
    struct Extras {
        std::vector<uint8_t> logo;              // logo image (PNG)
        std::vector<uint8_t> diorama;           // diorama model (.glb)
        std::vector<uint8_t> preview;           // the picture behind the route preview when there is no diorama (PNG)
    };

    struct Converted {
        rush2::track2049::ConvertedTrack track;
        MenuArt menu;
        Settings settings;
        std::vector<Texture> textures;          // the track's, then the authored diorama's (Texture::diorama)
        std::array<float, 16> custom_drag{};    // drag coefficient per material slot, for the custom surfaces
        int checkpoints = 0;                    // CHECKPOINT_n count (the finish line not counted)
        float lap_feet = 0.0f;                  // length of the AI path's loop
    };

    // prefix: the host slot's track prefix ("HAWAII"). Messages go to report. Errors in it don't stop the track (a
    // missing or broken AI path or FINISH gets a stand-in, what is past the game's limits is left out): false only
    // when track.glb can't be read, there is no collision or nothing at all to draw, or the collision is too dense for
    // the game's index.
    bool convert(const std::vector<uint8_t>& glb, const std::string& toml_text, const std::string& prefix,
                 Converted& out, Report& report, const Extras& extras = {});

    // Track select art (src/track_mod/track_mod_menu.cpp, the game only). make_menu_art completes art (which the
    // conversion gave the author's diorama, if any) with the logo and, without a diorama, the route preview.
    // serialize / deserialize: for the disk cache. The names are those of the track's art in asset 3.
    void make_menu_art(const Converted& track, const Extras& extras, MenuArt& art, Report& report);
    std::vector<uint8_t> serialize(const MenuArt& art);
    bool deserialize(const std::vector<uint8_t>& bytes, MenuArt& art);
    std::string diorama_model_name(int index);
    std::string logo_texture_name(int index);
}

#endif
