#ifndef __DATA_FILES_H__
#define __DATA_FILES_H__

// The recomp's own JSON files (src/data_files.cpp). Each is one object of named sections, a section per feature, so
// features share a file without knowing each other's layout:
//
// - Saves: saves/rush2.n64.us.json, next to the runtime's Controller Pak image (saves/rush2.n64.us.mpk, src/pak.cpp).
//   Everything a player earns or sets up that the pak's records have no room for.
//     "cars"          2049 car options and each record's selected car (src/rush2049/car2049.cpp)
//     "collectibles"  SF Rush keys, 2049 coins and unlock purchases per profile (src/collectibles.cpp)
//     "records"       2049 and SF Rush times and stats per profile (src/rush2049/track2049_records.cpp)
//     "track_select"  the added track last chosen on the track selects (src/rush2049/track2049_menu.cpp)
//   Ghost recordings are files of their own in saves/ghosts (src/ghost.cpp).
// - Players: players.json in the config folder.
//     "controllers"   each player's controller and the keyboard's player (src/input.cpp)
//     "bindings"      each player's controller and keyboard bindings (src/controls.cpp)
//
// Sections are passed as JSON text: the executable has two nlohmann::json versions on its include path (see
// src/rush2049/wings.cpp), so a json type can't cross files.

#include <filesystem>
#include <string>

namespace rush2::data_files {
    enum class File { Saves, Players };

    // The folder of the runtime's save file.
    std::filesystem::path save_folder();

    // A section's JSON text, or "" if the file or the section is missing.
    std::string read(File file, const std::string& section);
    // Replaces a section and writes the file (through a temporary file; the previous one is kept as .bak). Returns
    // false if json isn't valid JSON or the file couldn't be written.
    bool write(File file, const std::string& section, const std::string& json);

    // Moves the files of older versions into the layout above: car2049.json, collectibles.json,
    // track2049_records.json, track2049.json and the ghosts folder from the app folder into saves, bindings.json into
    // players.json, and rush2049.json and rush1.json into games.json (src/rush2049/wings.cpp). Call at startup, after the config
    // path is registered and before any config is loaded.
    void migrate();
}

#endif
