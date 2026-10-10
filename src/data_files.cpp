// The recomp's own JSON files and the migration of older versions' files (include/data_files.h).

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <system_error>
#include <vector>

#include "json/json.hpp"

#include "librecomp/game.hpp"
#include "util/file.h"

#include "data_files.h"

namespace fs = std::filesystem;
using nlohmann::json;

namespace {
    constexpr int file_count = 2;
    constexpr int layout_version = 1;

    struct State {
        json root = json::object();
        bool loaded = false;
    };
    std::mutex files_mutex;
    State states[file_count];

    fs::path app_folder() {
        return recomp::get_config_path();
    }

    fs::path path_of(rush2::data_files::File file) {
        // The save file is named after the game id, as the runtime's own save file and the pak image are.
        return file == rush2::data_files::File::Saves ? rush2::data_files::save_folder() / "rush2.n64.us.json"
                                                      : app_folder() / "players.json";
    }

    fs::path with_suffix(fs::path path, const char* suffix) {
        path += suffix;
        return path;
    }

    // Reads a JSON object from path, or from its .bak if path is missing or unreadable.
    bool read_json(const fs::path& path, json& out) {
        for (const fs::path& candidate : { path, with_suffix(path, ".bak") }) {
            std::ifstream f{ candidate };
            if (!f) {
                continue;
            }
            json j = json::parse(f, nullptr, false);
            if (j.is_object()) {
                out = std::move(j);
                return true;
            }
        }
        return false;
    }

    bool write_json(const fs::path& path, const json& j, int indent) {
        std::error_code ec;
        fs::create_directories(path.parent_path(), ec);
        fs::path temp = with_suffix(path, ".tmp");
        {
            std::ofstream f{ temp, std::ios::trunc };
            f << j.dump(indent) << "\n";
            f.close(); // Flushes, so a full disk fails here rather than after the rename.
            if (f.fail()) {
                printf("[Data] Couldn't write %s\n", temp.string().c_str());
                return false;
            }
        }
        if (fs::exists(path, ec)) {
            fs::rename(path, with_suffix(path, ".bak"), ec);
        }
        fs::rename(temp, path, ec);
        if (ec) {
            printf("[Data] Couldn't replace %s: %s\n", path.string().c_str(), ec.message().c_str());
            return false;
        }
        return true;
    }

    State& state_of(rush2::data_files::File file) {
        State& s = states[static_cast<int>(file)];
        if (!s.loaded) {
            s.loaded = true;
            fs::path path = path_of(file);
            std::error_code ec;
            if (!read_json(path, s.root) && fs::exists(path, ec)) {
                // Kept aside, since the next write replaces the file.
                printf("[Data] Couldn't read %s\n", path.string().c_str());
                fs::rename(path, with_suffix(path, ".corrupt"), ec);
            }
        }
        return s;
    }

    bool write_state(rush2::data_files::File file, State& s) {
        s.root["version"] = layout_version;
        return write_json(path_of(file), s.root, 2);
    }

    void remove_with_backup(const fs::path& path) {
        std::error_code ec;
        fs::remove(path, ec);
        fs::remove(with_suffix(path, ".bak"), ec);
        fs::remove(with_suffix(path, ".tmp"), ec);
    }

    // Side saves that were files of their own in the app folder, each now a section of the save file.
    void migrate_saves() {
        struct Legacy {
            const char* name;
            const char* section;
        };
        constexpr Legacy legacy[] = { { "car2049.json", "cars" },
                                      { "collectibles.json", "collectibles" },
                                      { "track2049_records.json", "records" },
                                      { "track2049.json", "track_select" } };
        State& s = state_of(rush2::data_files::File::Saves);
        std::vector<fs::path> merged;
        bool changed = false;
        for (const Legacy& l : legacy) {
            fs::path path = app_folder() / l.name;
            std::error_code ec;
            if (!fs::exists(path, ec)) {
                continue;
            }
            json j;
            if (!read_json(path, j)) {
                // Left where it is for the user to look at.
                printf("[Data] Couldn't read %s\n", path.string().c_str());
                continue;
            }
            if (!s.root.contains(l.section)) {
                s.root[l.section] = std::move(j);
                changed = true;
            }
            merged.push_back(path);
        }
        if (changed && !write_state(rush2::data_files::File::Saves, s)) {
            return;
        }
        for (const fs::path& path : merged) {
            remove_with_backup(path);
        }
    }

    void migrate_ghosts() {
        fs::path old_folder = app_folder() / "ghosts";
        fs::path new_folder = rush2::data_files::save_folder() / "ghosts";
        std::error_code ec;
        if (!fs::is_directory(old_folder, ec)) {
            return;
        }
        fs::create_directories(new_folder, ec);
        std::vector<fs::path> files;
        for (const auto& entry : fs::directory_iterator(old_folder, ec)) {
            files.push_back(entry.path());
        }
        for (const fs::path& file : files) {
            fs::path target = new_folder / file.filename();
            if (!fs::exists(target, ec)) {
                fs::rename(file, target, ec);
            }
        }
        // Only goes if every file moved.
        fs::remove(old_folder, ec);
    }

    // bindings.json and the old players.json (the controllers alone, a "players" list at the top) as sections of
    // players.json.
    void migrate_players() {
        fs::path bindings_path = app_folder() / "bindings.json";
        State& s = state_of(rush2::data_files::File::Players);
        bool changed = false;
        if (s.root.contains("players")) {
            json controllers = std::move(s.root);
            s.root = json::object();
            s.root["controllers"] = std::move(controllers);
            changed = true;
        }
        std::error_code ec;
        bool had_bindings = fs::exists(bindings_path, ec);
        json bindings;
        if (had_bindings && !s.root.contains("bindings") && read_json(bindings_path, bindings)) {
            s.root["bindings"] = std::move(bindings);
            changed = true;
        }
        if (changed && !write_state(rush2::data_files::File::Players, s)) {
            return;
        }
        if (had_bindings) {
            remove_with_backup(bindings_path);
        }
    }

    // The Rush 2049 and SF Rush options (rush2049.json, rush1.json) as one config, games.json (src/rush2049/wings.cpp), with
    // ids that say which game they belong to.
    void migrate_games_config() {
        struct Renamed {
            const char* file;
            const char* from;
            const char* to;
        };
        constexpr Renamed renamed[] = { { "rush2049.json", "wings", "wings" },
                                        { "rush2049.json", "tracks", "rush2049_tracks" },
                                        { "rush2049.json", "cars", "rush2049_cars" },
                                        { "rush2049.json", "computer_cars", "rush2049_computer_cars" },
                                        { "rush2049.json", "car_speeds", "car_speeds" },
                                        { "rush2049.json", "wing_style_p1", "wing_style_p1" },
                                        { "rush2049.json", "wing_style_p2", "wing_style_p2" },
                                        { "rush1.json", "tracks", "sfrush_tracks" } };
        fs::path target = app_folder() / "games.json";
        std::error_code ec;
        json existing;
        if (!read_json(target, existing)) {
            json merged = json::object();
            for (const char* name : { "rush2049.json", "rush1.json" }) {
                json j;
                if (!read_json(app_folder() / name, j)) {
                    continue;
                }
                for (const Renamed& r : renamed) {
                    if (std::string(r.file) == name && j.contains(r.from)) {
                        merged[r.to] = j[r.from];
                    }
                }
            }
            // As librecomp writes its configs.
            if (!merged.empty() && !write_json(target, merged, 4)) {
                return;
            }
        }
        for (const char* name : { "rush2049.json", "rush1.json" }) {
            remove_with_backup(app_folder() / name);
        }
    }
}

fs::path rush2::data_files::save_folder() {
    return app_folder() / "saves";
}

std::string rush2::data_files::read(File file, const std::string& section) {
    std::lock_guard lock{ files_mutex };
    State& s = state_of(file);
    auto it = s.root.find(section);
    return it != s.root.end() ? it->dump() : std::string{};
}

bool rush2::data_files::write(File file, const std::string& section, const std::string& text) {
    json j = json::parse(text, nullptr, false);
    if (j.is_discarded()) {
        return false;
    }
    std::lock_guard lock{ files_mutex };
    State& s = state_of(file);
    s.root[section] = std::move(j);
    return write_state(file, s);
}

void rush2::data_files::migrate() {
    std::lock_guard lock{ files_mutex };
    migrate_saves();
    migrate_ghosts();
    migrate_players();
    migrate_games_config();
}
