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

// The San Francisco Rush 2049 arcade (SE) content (src/arcade2049/, docs/arcade2049.md): Alcatraz, the arcade-only
// track, and the arcade's cars, built from the arcade data ProjectR unpacks from the arcade hard drive into two mods
// (src/arcade2049/arcade_track.cpp, arcade_cars.cpp). The Games tab builds and installs them; a new
// build or new arcade data rebuilds them once at the next launch.
namespace rush2::arcade2049 {
    inline constexpr const char* track_mod_id = "rush2049_arcade";
    inline constexpr const char* car_mod_id = "rush2049_arcade_cars";

    // ProjectR's folder for the unpacked arcade disk: ARCADE2049_DATA, else %LOCALAPPDATA%\ProjectR\Data\sf2049se
    // (Windows) or $XDG_DATA_HOME (~/.local/share)/ProjectR/Data/sf2049se.
    std::filesystem::path default_data_dir();
    // Whether `dir` holds what the mods are built from (TRACKS/TRACK4 and the CARS folders).
    bool is_data_dir(const std::filesystem::path& dir);
    // The data folder `picked` (any file or folder inside it) is in, if any.
    std::optional<std::filesystem::path> find_data_dir(const std::filesystem::path& picked);

    // The arcade tracks the track mod can hold: Alcatraz (arcade only) and, with the Alcatraz Only option off, the
    // arcade's own versions of the six Rush 2049 tracks, as tracks of their own (tracks/<id>/ in the mod).
    struct ArcadeTrack {
        const char* folder;   // TRACKS/<folder>
        const char* id;
        const char* name;
    };
    // In track select order: the N64 tracks' (Marina, Haight, Civic, Metro, Mission, Presidio), Alcatraz last, all after
    // the Rush 2049 tracks (each track.toml's [menu] after names the one before).
    inline constexpr ArcadeTrack arcade_tracks[] = {
        { "TRACK1", "marina_arcade", "Marina Arcade" },
        { "TRACK2", "haight_arcade", "Haight Arcade" },
        { "TRACK3", "civic_arcade", "Civic Arcade" },
        { "TRACK5", "metro_arcade", "Metro Arcade" },
        { "TRACK6", "mission_arcade", "Mission Arcade" },
        { "TRACK7", "presidio_arcade", "Presidio Arcade" },
        { "TRACK4", "alcatraz", "Alcatraz" },
    };

    // The two mods' files by their path in the mod. `cancelled` is polled between steps; the log gets what each
    // conversion did. assets_dir holds each track's <id>_logo.png and <id>_preview.png (made by
    // tools/build_banners.py --arcade). alcatraz_only: just Alcatraz, else every arcade_tracks entry.
    using ModFiles = std::map<std::string, std::vector<uint8_t>>;
    bool build_track_mod(const std::filesystem::path& data_dir, const std::filesystem::path& assets_dir, ModFiles& out,
                         std::string& log, bool alcatraz_only = true, const std::function<bool()>& cancelled = {});
    bool build_car_mod(const std::filesystem::path& data_dir, ModFiles& out, std::string& log,
                       const std::function<bool()>& cancelled = {});

    // The game (src/arcade2049/arcade_install.cpp).
    // The Games tab's Rush 2049 Arcade section: its status and the Build and Install / Select Folder buttons.
    void add_games_section(rush2::ui::OptionsPage* page, std::function<void()>& refresh);
    // The launch's build pass (src/prewarm.cpp), before the game starts: installed arcade mods built by another build
    // or from other data are rebuilt and installed again now, on the calling thread.
    void refresh_now();
    // A mod has been built this session (by refresh_now or the Games tab): the launch's progress screen shows.
    bool rebuilt();

    // The Games tab's options under Rush 2049 Arcade: Alcatraz Only (arcade_install.cpp) and Arcade Checkpoints.
    void init_config();
    void load_config();   // after rush2::wings::load_config()
    void add_options(rush2::ui::OptionsPage* page);

    // The Arcade Checkpoints option (src/arcade2049/arcade_checkpoints.cpp): the arcade's CHECKPOINT banner, from the
    // arcade tracks mod, over the Rush 2049 tracks' checkpoints.
    void init_checkpoints_config(recomp::config::Config& config);
    void load_checkpoints_config();
    void add_checkpoints_option(rush2::ui::OptionsPage* page);
    void checkpoints_changed();                 // the arcade tracks mod was installed again: read the banner again
    // checkpoints_frame (every game frame) is in include/arcade_checkpoints.h, which needs the game's headers.
}

#endif
