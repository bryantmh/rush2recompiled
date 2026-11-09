#ifndef __ARCADE2049_H__
#define __ARCADE2049_H__

#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>


namespace recomp::config {
    class Config;
}

namespace rush2::ui {
    class OptionsPage;
}

namespace rush2::track_mod {
    struct Extras;
}

// The arcade games' content (src/arcade2049/, docs/arcade2049.md, docs/sfrush_arcade.md), built from the arcade data
// ProjectR unpacks from an arcade hard drive into mods (src/arcade2049/arcade_track.cpp, arcade_cars.cpp). The Games
// tab builds and installs them; a new build or new arcade data rebuilds them once at the next launch.
//   - San Francisco Rush 2049 (SE): Alcatraz, the arcade-only track, the arcade's versions of the six Rush 2049 race
//     tracks, and the arcade's cars (two mods).
//   - San Francisco Rush: The Rock: the arcade's versions of SF Rush's seven tracks (one mod).
namespace rush2::arcade2049 {
    inline constexpr const char* track_mod_id = "rush2049_arcade";
    inline constexpr const char* car_mod_id = "rush2049_arcade_cars";
    inline constexpr const char* sfrush_track_mod_id = "sfrush_arcade";

    // The arcade tracks a track mod holds (tracks/<id>/ in the mod). Each arcade version of an N64 course is a version
    // of it (track.toml [menu] variant_of, variant = "ARCADE": the track select's version button switches to it).
    struct ArcadeTrack {
        const char* folder;   // TRACKS/<folder>
        const char* id;
        const char* name;
        const char* course;   // the course it is a version of (track.toml [menu] variant_of), or null
    };
    inline constexpr ArcadeTrack arcade_tracks[] = {
        { "TRACK1", "marina_arcade", "Marina Arcade", "rush2049:marina" },
        { "TRACK2", "haight_arcade", "Haight Arcade", "rush2049:haight" },
        { "TRACK3", "civic_arcade", "Civic Arcade", "rush2049:civic" },
        { "TRACK5", "metro_arcade", "Metro Arcade", "rush2049:metro" },
        { "TRACK6", "mission_arcade", "Mission Arcade", "rush2049:mission" },
        { "TRACK7", "presidio_arcade", "Presidio Arcade", "rush2049:presidio" },
        { "TRACK4", "alcatraz", "Alcatraz", nullptr },
    };
    // Rush The Rock numbers its tracks as the N64 game doesn't: its TRACK4 is The Rock (N64 track 7) and TRACK5-7 are
    // N64 tracks 4-6 (matched by their drone paths, docs/sfrush_arcade.md).
    inline constexpr ArcadeTrack sfrush_tracks[] = {
        { "TRACK1", "golden_gate_arcade", "Golden Gate Arcade", "sfrush:golden_gate" },
        { "TRACK2", "embarcadero_arcade", "Embarcadero Arcade", "sfrush:embarcadero" },
        { "TRACK3", "market_arcade", "Market Arcade", "sfrush:market" },
        { "TRACK5", "downtown_arcade", "Downtown Arcade", "sfrush:downtown" },
        { "TRACK6", "the_heights_arcade", "The Heights Arcade", "sfrush:the_heights" },
        { "TRACK7", "sunset_arcade", "Sunset Arcade", "sfrush:sunset" },
        { "TRACK4", "the_rock_arcade", "The Rock Arcade", "sfrush:the_rock" },
    };

    // An arcade game whose data the Games tab builds mods from.
    struct ArcadeGame {
        const char* key;             // "rush2049", "sfrush": the converter's per-game rules (arcade_track.cpp)
        const char* title;           // the Games tab section's heading
        const char* data_folder;     // ProjectR's folder for its unpacked disk (sf2049se, sfrushrk)
        const char* data_env;        // the environment variable that overrides that folder
        const char* check_file;      // a file only this game's data folder has, relative to it (Rush The Rock has
                                     // CARS/CAR8 too, but no TRACK8; Rush 2049 SE has no MIRROR tracks)
        const char* track_mod_id;
        const char* track_mod_name;  // mod.json display_name
        const char* car_mod_id;      // null: no car mod
        const char* assets;          // assets/<assets>/: each track's <id>_logo.png (and <id>_preview.png)
        const ArcadeTrack* tracks;
        size_t track_count;
        bool is_2049() const { return key[0] == 'r'; }
    };
    inline constexpr ArcadeGame rush2049_arcade{
        "rush2049", "Rush 2049 Arcade", "sf2049se", "ARCADE2049_DATA", "TRACKS/TRACK8/roaddat.rom", track_mod_id,
        "Rush 2049 Arcade Tracks", car_mod_id, "arcade2049", arcade_tracks, std::size(arcade_tracks) };
    inline constexpr ArcadeGame sfrush_arcade{
        "sfrush", "SF Rush Arcade", "sfrushrk", "SFRUSH_ARCADE_DATA", "TRACKS/MIRROR7/roaddat.rom", sfrush_track_mod_id,
        "SF Rush Arcade Tracks", nullptr, "sfrush_arcade", sfrush_tracks, std::size(sfrush_tracks) };
    inline constexpr const ArcadeGame* arcade_games[] = { &rush2049_arcade, &sfrush_arcade };

    // ProjectR's folder for the game's unpacked arcade disk: its data_env, else %LOCALAPPDATA%\ProjectR\Data\<folder>
    // (Windows) or $XDG_DATA_HOME (~/.local/share)/ProjectR/Data/<folder>.
    std::filesystem::path default_data_dir(const ArcadeGame& game = rush2049_arcade);
    // Whether `dir` holds what the game's mods are built from (its tracks' folders and its check_file).
    bool is_data_dir(const std::filesystem::path& dir, const ArcadeGame& game = rush2049_arcade);
    // The game's data folder `picked` (any file or folder inside it) is in, if any.
    std::optional<std::filesystem::path> find_data_dir(const std::filesystem::path& picked,
                                                       const ArcadeGame& game = rush2049_arcade);

    // The mods' files by their path in the mod. `cancelled` is polled between steps; the log gets what each
    // conversion did. assets_dir holds each track's <id>_logo.png and <id>_preview.png (made by
    // tools/build_banners.py --arcade). Every track of the game.
    using ModFiles = std::map<std::string, std::vector<uint8_t>>;
    bool build_track_mod(const ArcadeGame& game, const std::filesystem::path& data_dir,
                         const std::filesystem::path& assets_dir, ModFiles& out, std::string& log,
                         const std::function<bool()>& cancelled = {});
    bool build_car_mod(const std::filesystem::path& data_dir, ModFiles& out, std::string& log,
                       const std::function<bool()>& cancelled = {});

    // The game (src/arcade2049/arcade_install.cpp).
    // The Games tab's section of an arcade game: its status and the Build and Install / Select Folder buttons.
    void add_games_section(rush2::ui::OptionsPage* page, std::function<void()>& refresh,
                           const ArcadeGame& game = rush2049_arcade);
    // The launch's build pass (src/prewarm.cpp), before the game starts: installed arcade mods built by another build
    // or from other data are rebuilt and installed again now, on the calling thread (every game's).
    void refresh_now();
    // A mod has been built this session (by refresh_now or the Games tab): the launch's progress screen shows.
    bool rebuilt();
    // The game's arcade data folder in use (the one picked this session, else the installed mods', else ProjectR's),
    // or empty when there is none: the music plays from it (src/music_streams.cpp). Any thread.
    std::filesystem::path current_data_dir(const ArcadeGame& game);

    // The Games tab's options under Rush 2049 Arcade: Arcade Checkpoints.
    void init_config();
    void load_config();   // after rush2::wings::load_config()
    void add_options(rush2::ui::OptionsPage* page);

    // The Arcade Checkpoints option (src/arcade2049/arcade_checkpoints.cpp): the arcade's CHECKPOINT banner, from the
    // arcade tracks mod, over the Rush 2049 tracks' checkpoints.
    void init_checkpoints_config(recomp::config::Config& config);
    void load_checkpoints_config();
    void add_checkpoints_option(rush2::ui::OptionsPage* page);
    void checkpoints_changed();                 // the arcade tracks mod was installed again: read the banner again

    // The coins or keys of the course an arcade version of it has (src/arcade2049/arcade_collectibles.cpp,
    // docs/arcade2049.md#coins-and-keys-on-the-arcade-versions), added to `out` for the custom track converter: their
    // models from the N64 game and their records. False when the track has none or that game's ROM isn't there.
    bool collectible_extras(const std::string& mod_id, const std::string& folder, rush2::track_mod::Extras& out);
    // checkpoints_frame (every game frame) is in include/arcade_checkpoints.h, which needs the game's headers.
}

#endif
