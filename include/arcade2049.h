#ifndef __ARCADE2049_H__
#define __ARCADE2049_H__

#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace rush2::ui {
    class OptionsPage;
}

// The San Francisco Rush 2049 arcade (SE) content (src/arcade2049/, docs/arcade2049.md): Alcatraz, the arcade-only
// track, and the arcade's cars, built from the arcade data ProjectR unpacks from the arcade hard drive into two mods,
// the same ones tools/arcade2049/track_mod.py and car_mod.py write. The Games tab builds and installs them; a new
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

    // The two mods' files by their path in the mod. `cancelled` is polled between steps; the log gets what each
    // conversion did (as the Python tools print). assets_dir holds alcatraz_logo.png and alcatraz_preview.png.
    using ModFiles = std::map<std::string, std::vector<uint8_t>>;
    bool build_track_mod(const std::filesystem::path& data_dir, const std::filesystem::path& assets_dir, ModFiles& out,
                         std::string& log, const std::function<bool()>& cancelled = {});
    bool build_car_mod(const std::filesystem::path& data_dir, ModFiles& out, std::string& log,
                       const std::function<bool()>& cancelled = {});

    // The game (src/arcade2049/arcade_install.cpp).
    // The Games tab's Rush 2049 Arcade section: its status and the Build and Install / Select Folder buttons.
    void add_games_section(rush2::ui::OptionsPage* page, std::function<void()>& refresh);
    // Once the launcher is up: installed arcade mods built by another build or from other data are rebuilt in the
    // background.
    void start_refresh();
}

#endif
