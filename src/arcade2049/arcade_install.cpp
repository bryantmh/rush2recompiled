// Builds and installs the arcade mods (include/arcade2049.h: Rush 2049 SE's tracks and cars, Rush The Rock's tracks)
// from the Games tab, on a worker thread, each arcade game with its own section and state: each
// mod is written as <mods>/<id>.rtz (a zip of the files build_track_mod and build_car_mod make), its mod.json
// carrying "rush2_arcade": the key of what it was built from (the input files, the shipped assets and this build)
// and the data folder. At launch, an installed mod whose key no longer matches (a new build, new arcade data) is
// built again once from that folder, keeping whether it was on. A loose mod folder of the same id (an install by the
// Python converters this replaced, removed 2026-10-20) is replaced only by the button.

#include <atomic>
#include <chrono>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <thread>

#include "json/json.hpp"
#include "librecomp/mods.hpp"
#include "recompui/recompui.h"
#include "util/file.h"
#include "elements/ui_button.h"
#include "elements/ui_label.h"

#include "arcade2049.h"
#include "car_mods.h"
#include "options_page.h"
#include "texture_upscale.h"
#include "track2049.h"
#include "track_cache.h"
#include "wings_internal.h"

namespace a49 = rush2::arcade2049;

namespace {
    std::filesystem::path assets_dir(const a49::ArcadeGame& game) {
        return recompui::file::get_program_path() / "assets" / game.assets;
    }

    // The arcade folders a mod of the game reads.
    std::vector<std::filesystem::path> inputs(const a49::ArcadeGame& game, const std::filesystem::path& data, bool cars) {
        std::vector<std::filesystem::path> out;
        if (!cars) {
            for (size_t k = 0; k < game.track_count; k++) out.push_back(data / "TRACKS" / game.tracks[k].folder);
            return out;
        }
        for (const char* car : { "CAR2", "CAR4", "CAR5", "CAR6", "CAR7", "CAR8", "CAR10", "CAR11" }) out.push_back(data / "CARS" / car);
        return out;
    }

    std::string compute_source_key(const a49::ArcadeGame& game, const std::filesystem::path& data, bool cars);

    // What a mod is built from: its input files' names and contents, the shipped assets and this build. Worked out once
    // per data folder and mod (it reads every input file), for the refresh and a race's check.
    std::string source_key(const a49::ArcadeGame& game, const std::filesystem::path& data, bool cars) {
        static std::mutex memo_mutex;
        static std::map<std::string, std::string> memo;
        std::string id = rush2::upscale::path_utf8(data) + "|" + game.key + (cars ? "|cars" : "|tracks");
        std::lock_guard lock{ memo_mutex };
        auto it = memo.find(id);
        if (it != memo.end()) return it->second;
        return memo[id] = compute_source_key(game, data, cars);
    }

    std::string compute_source_key(const a49::ArcadeGame& game, const std::filesystem::path& data, bool cars) {
        uint64_t seed = cars ? 0xCA45ull : game.is_2049() ? 0x7AC4ull : 0x5F7Aull;
        uint64_t h = rush2::track_cache::hash(nullptr, 0, rush2::track_cache::build() ^ seed);
        std::vector<std::filesystem::path> files;
        std::error_code ec;
        for (const auto& dir : inputs(game, data, cars)) {
            for (auto it = std::filesystem::recursive_directory_iterator(dir, ec); !ec && it != std::filesystem::recursive_directory_iterator(); it.increment(ec)) {
                if (it->is_regular_file(ec)) files.push_back(it->path());
            }
        }
        if (!cars) {
            for (size_t k = 0; k < game.track_count; k++) {
                for (const char* file : { "_logo.png", "_preview.png" }) {
                    std::filesystem::path f = assets_dir(game) / (std::string(game.tracks[k].id) + file);
                    if (std::filesystem::exists(f, ec)) files.push_back(f);
                }
            }
        }
        std::sort(files.begin(), files.end());
        // Reading every file took about a second of each launch: the key is kept on disk under the files' names, sizes
        // and times, and worked out again only when one of those changes.
        uint64_t stamp = h;
        for (const auto& f : files) {
            std::string name = rush2::upscale::path_utf8(f.lexically_relative(data));
            stamp = rush2::track_cache::hash(reinterpret_cast<const uint8_t*>(name.data()), name.size(), stamp);
            uint64_t size_time[2] = { (uint64_t)std::filesystem::file_size(f, ec),
                                      (uint64_t)std::filesystem::last_write_time(f, ec).time_since_epoch().count() };
            stamp = rush2::track_cache::hash(reinterpret_cast<const uint8_t*>(size_time), sizeof(size_time), stamp);
        }
        const std::string blob = std::string("arcade_source_key_") + (cars ? "cars" : game.is_2049() ? "tracks" : "sfrush_tracks");
        std::vector<uint8_t> kept;
        if (rush2::track_cache::load_blob(blob, stamp, kept) && kept.size() == 16) return std::string(kept.begin(), kept.end());
        for (const auto& f : files) {
            std::string name = rush2::upscale::path_utf8(f.lexically_relative(data));
            h = rush2::track_cache::hash(reinterpret_cast<const uint8_t*>(name.data()), name.size(), h);
            std::vector<uint8_t> bytes = rush2::upscale::read_file(f);
            h = rush2::track_cache::hash(bytes.data(), bytes.size(), h);
        }
        char text[17];
        snprintf(text, sizeof(text), "%016llx", (unsigned long long)h);
        rush2::track_cache::save_blob(blob, stamp, std::vector<uint8_t>(text, text + 16));
        return text;
    }

    // The installed copy of a mod: as the game built it (<id>.rtz with its key) or a loose folder of that id.
    struct Installed {
        bool rtz = false, loose = false;
        std::string key;
        std::filesystem::path data;
    };

    Installed installed(const std::string& id) {
        Installed out;
        std::filesystem::path mods = recomp::mods::get_mods_directory();
        std::vector<uint8_t> manifest;
        rush2::upscale::ZipReader zip(mods / (id + ".rtz"));
        if (zip.is_open() && zip.read("mod.json", manifest)) {
            out.rtz = true;
            nlohmann::json j = nlohmann::json::parse(manifest.begin(), manifest.end(), nullptr, false);
            if (j.is_object() && j.contains("rush2_arcade") && j["rush2_arcade"].is_object()) {
                out.key = j["rush2_arcade"].value("key", "");
                out.data = std::filesystem::u8path(j["rush2_arcade"].value("data", ""));
            }
        }
        std::vector<uint8_t> loose = rush2::upscale::read_file(mods / id / "mod.json");
        if (!loose.empty()) {
            nlohmann::json j = nlohmann::json::parse(loose.begin(), loose.end(), nullptr, false);
            out.loose = j.is_object() && j.value("id", "") == id;
        }
        return out;
    }

    // The mods a game builds: its track mod, then its car mod if it has one (id, whether it is the cars).
    std::vector<std::pair<std::string, bool>> mods_of(const a49::ArcadeGame& game) {
        std::vector<std::pair<std::string, bool>> out{ { game.track_mod_id, false } };
        if (game.car_mod_id) out.push_back({ game.car_mod_id, true });
        return out;
    }

    // Per game: what its Games tab section shows and whether it is building.
    struct GameState {
        std::mutex mutex;
        std::string text;                    // what the section shows; written by the worker
        std::atomic<int> generation = 0;     // counts changes, so the page reads the note again only then
        std::atomic<bool> busy = false;
        std::filesystem::path chosen_data;   // the folder Select Folder picked, this session
    };
    GameState game_states[std::size(a49::arcade_games)];
    std::atomic<bool> rebuilding = false;    // a mod has been built this session (a49::rebuilt)

    GameState& state_of(const a49::ArcadeGame& game) {
        for (size_t k = 0; k < std::size(a49::arcade_games); k++) {
            if (a49::arcade_games[k] == &game) return game_states[k];
        }
        return game_states[0];
    }

    void set_state(const a49::ArcadeGame& game, const std::string& text) {
        GameState& s = state_of(game);
        std::lock_guard lock{ s.mutex };
        s.text = text;
        s.generation++;
    }

    std::string state(const a49::ArcadeGame& game) {
        GameState& s = state_of(game);
        std::lock_guard lock{ s.mutex };
        return s.text;
    }

    // Writes the mod as <mods>/<id>.rtz in place of any copy of it, then has the game read it again. enable: turn
    // it on (the button); otherwise it keeps whether it was on.
    bool install(const std::string& id, a49::ModFiles& files, const std::string& key, const std::filesystem::path& data,
                 bool enable, std::string& error) {
        nlohmann::json manifest = nlohmann::json::parse(files["mod.json"].begin(), files["mod.json"].end());
        manifest["rush2_arcade"] = { { "key", key }, { "data", rush2::upscale::path_utf8(data) } };
        std::string text = manifest.dump(4);
        files["mod.json"].assign(text.begin(), text.end());
        std::filesystem::path mods = recomp::mods::get_mods_directory();
        std::error_code ec;
        std::filesystem::create_directories(mods, ec);
        std::filesystem::path pack = mods / (id + ".rtz"), fresh = mods / (id + ".rtz.new");
        rush2::upscale::ZipWriter zip;
        for (const auto& [name, bytes] : files) {
            if (!zip.add(name, bytes)) {
                error = "couldn't pack " + name;
                return false;
            }
        }
        if (!zip.write(fresh)) {
            error = "couldn't write " + rush2::upscale::path_utf8(fresh);
            std::filesystem::remove(fresh, ec);
            return false;
        }
        recomp::mods::close_mods();
        // A loose folder of this id (an install by the old Python converters) would be a second mod of the same id.
        if (installed(id).loose) std::filesystem::remove_all(mods / id, ec);
        bool ok = false;
        for (int attempt = 0; attempt < 200 && !ok; attempt++) {
            ec.clear();
            std::filesystem::rename(fresh, pack, ec);
            ok = !ec;
            if (!ok) std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        if (!ok) {
            error = "the installed mod is in use";
            std::filesystem::remove(fresh, ec);
        }
        recomp::mods::scan_mods();
        if (ok && enable) recomp::mods::enable_mod(id, true);
        recompui::update_mod_list(false);
        return ok;
    }

    // Builds and installs the game's mods from `data`. refresh: only those installed by the game whose key changed.
    void run(const a49::ArcadeGame& game, std::filesystem::path data, bool refresh) {
        std::string error, log;
        int built = 0;
        bool cars_built = false;
        for (const auto& [id, cars] : mods_of(game)) {
            std::string key = source_key(game, data, cars);
            if (refresh) {
                Installed have = installed(id);
                if (!have.rtz || have.key.empty() || have.key == key) continue;
            }
            rebuilding = true;
            set_state(game, cars ? "Building the cars..." : "Building the arcade tracks...");
            a49::ModFiles files;
            bool ok = cars ? a49::build_car_mod(data, files, log)
                           : a49::build_track_mod(game, data, assets_dir(game), files, log);
            if (!ok || !install(id, files, key, data, !refresh, error)) {
                if (error.empty()) error = "the arcade data couldn't be read";
                printf("[arcade] %s: %s\n%s", id.c_str(), error.c_str(), log.c_str());
                set_state(game, "Failed: " + error);
                return;
            }
            built++;
            cars_built = cars_built || cars;
        }
        printf("[arcade] %s: %s\n%s", game.key, built ? "installed" : "up to date", log.c_str());
        if (built) {
            if (cars_built) rush2::car_mods::reload();
            rush2::track2049::mods_changed();
            if (game.is_2049()) a49::checkpoints_changed();
        }
        if (!refresh) set_state(game, game.car_mod_id ? "Installed: the arcade tracks and cars" : "Installed: the arcade tracks");
        else if (built) set_state(game, "Rebuilt");
    }

    // The Games tab's buttons: builds and installs the game's mods on a worker thread (a press while one builds does
    // nothing).
    bool start(const a49::ArcadeGame& game, const std::filesystem::path& data) {
        GameState& s = state_of(game);
        if (s.busy.exchange(true)) {
            return false;
        }
        // Detached: a build still running when the game closes leaves at most a .rtz.new behind.
        std::thread([&game, &s, data]() {
            run(game, data, false);
            s.busy = false;
            s.generation++;
        }).detach();
        return true;
    }

    // The data folder to build from: the one picked this session, else the installed mods', else ProjectR's.
    std::filesystem::path data_dir(const a49::ArcadeGame& game) {
        const std::filesystem::path& chosen = state_of(game).chosen_data;
        if (!chosen.empty() && a49::is_data_dir(chosen, game)) return chosen;
        for (const auto& [id, cars] : mods_of(game)) {
            Installed have = installed(id);
            if (!have.data.empty() && a49::is_data_dir(have.data, game)) return have.data;
        }
        std::filesystem::path d = a49::default_data_dir(game);
        return a49::is_data_dir(d, game) ? d : std::filesystem::path();
    }

    std::string status_note(const a49::ArcadeGame& game) {
        if (state_of(game).busy) return state(game);
        std::string s = state(game);
        if (!s.empty()) return s;
        Installed have = installed(game.track_mod_id);
        bool track = have.rtz || have.loose;
        if (data_dir(game).empty()) return track ? "Installed. Arcade data not found." : "Needs the arcade data (see the description).";
        return track ? "Installed." : "Arcade data found.";
    }

    // The section's description (the Games tab's right pane).
    std::string description(const a49::ArcadeGame& game) {
        if (game.is_2049()) {
            return "Adds Alcatraz, the track only the Rush 2049 arcade game has (listed after the Rush 2049 tracks), the "
                   "arcade's own versions of Marina, Haight, Civic, Metro, Mission and Presidio (the track select's version "
                   "button, X, Square or J on the keyboard, switches a course to ARCADE), and the arcade's Truck, Deuce and Caddy "
                   "with their paint styles, plus arcade paint styles for the GX-2, Magnum, Super GT, 8-Ball and Bruiser (the car "
                   "select's STYLE row).<br/><br/>"
                   "They're built from the arcade game's files, which ProjectR unpacks from a Rush 2049 Special Edition arcade "
                   "hard drive. If ProjectR has done that on this computer, <recomp-color primary>Build and Install</recomp-color> "
                   "finds them; otherwise press <recomp-color primary>Select Folder</recomp-color> and pick any file in the "
                   "unpacked folder (sf2049se).<br/><br/>"
                   "They install as two mods, Rush 2049 Arcade Tracks and Rush 2049 Arcade Cars, which the Mods menu can turn off. "
                   "Building takes a few seconds and you can keep playing; they show up the next time you're in the menus.";
        }
        return "Adds the San Francisco Rush arcade game's own versions of all seven SF Rush tracks, with the arcade's "
               "sharper textures and extra scenery. They share each course's place in the track select: the version "
               "button (X, Square or J on the keyboard) switches a course to ARCADE.<br/><br/>"
               "They're built from the arcade game's files, which ProjectR unpacks from a San Francisco Rush: The Rock "
               "arcade hard drive. If ProjectR has done that on this computer, <recomp-color primary>Build and "
               "Install</recomp-color> finds them; otherwise press <recomp-color primary>Select Folder</recomp-color> and "
               "pick any file in the unpacked folder (sfrushrk).<br/><br/>"
               "They install as a mod, SF Rush Arcade Tracks, which the Mods menu can turn off. Building takes a few "
               "seconds and you can keep playing; they show up the next time you're in the menus.";
    }
}

void a49::add_games_section(rush2::ui::OptionsPage* page, std::function<void()>& refresh, const ArcadeGame& game) {
    using namespace recompui;
    ContextId context = get_current_context();
    const std::string text = description(game);
    rush2::ui::OptionsPage::Heading heading = page->add_heading(game.title, status_note(game), text);
    Element* row = page->add_row(text);
    // Both buttons on one line at full size: the Games tab's column is a few pixels short of them at add_row's 16 gap.
    row->set_gap(12.0f);
    Button* build = context.create_element<Button>(row, "Build and Install", ButtonStyle::Secondary);
    build->add_pressed_callback([&game]() {
        std::filesystem::path data = data_dir(game);
        if (data.empty()) {
            message_box((std::string("The arcade files weren't found. Press Select Folder and pick any file in the folder "
                                     "ProjectR unpacked them to (") + game.data_folder + ").").c_str());
            return;
        }
        start(game, data);
    });
    Button* select = context.create_element<Button>(row, "Select Folder", ButtonStyle::Secondary);
    select->add_pressed_callback([&game]() {
        recompui::file::open_file_dialog([&game](bool success, const std::filesystem::path& path) {
            if (!success) return;
            std::optional<std::filesystem::path> data = find_data_dir(path, game);
            if (!data) {
                recompui::message_box((std::string("That isn't in the ") + game.title + " files. Pick a file in the folder "
                                       "ProjectR unpacked them to (" + game.data_folder + ", with TRACKS inside).").c_str());
                return;
            }
            state_of(game).chosen_data = *data;
            start(game, *data);
        });
    });
    GameState& s = state_of(game);
    refresh = [note = heading.note, &game, &s, seen = s.generation.load()]() mutable {
        int now = s.generation.load();
        if (now == seen) return;
        seen = now;
        note->set_text(status_note(game));
    };
}

// The Alcatraz Only option (arcade_alcatraz_only, removed 2026-11-05) built just Alcatraz unless it was off: the mod now
// always holds every arcade track, the six as versions of their courses. A games.json still holding it is ignored, and
// the launch's refresh builds the full mod once (its key no longer matches).
void a49::init_config() {
    init_checkpoints_config(rush2::wings::games_config());
}

void a49::load_config() {
    load_checkpoints_config();
}

void a49::add_options(rush2::ui::OptionsPage* page) {
    add_checkpoints_option(page);
}

std::filesystem::path a49::current_data_dir(const ArcadeGame& game) {
    return data_dir(game);
}

bool a49::rebuilt() {
    return rebuilding;
}

void a49::refresh_now() {
    static bool done = false;
    if (done) return;
    done = true;
    // Test aid: RUSH2_TEST_ARCADE_INSTALL=1 presses Build and Install at launch.
    bool test = false;
    if (const char* v = std::getenv("RUSH2_TEST_ARCADE_INSTALL"); v && *v == '1') test = true;
    for (const ArcadeGame* game : arcade_games) {
        // A game-built mod is rebuilt from the folder it was built from, or wherever data_dir finds the arcade files if
        // that one moved.
        bool built_by_game = false;
        for (const auto& [id, cars] : mods_of(*game)) {
            Installed have = installed(id);
            built_by_game = built_by_game || (have.rtz && !have.key.empty());
        }
        std::filesystem::path data = built_by_game || test ? data_dir(*game) : std::filesystem::path();
        if (data.empty()) continue;
        // A build already going (the Games tab's button) is waited for, then the check runs again on what it installed.
        GameState& s = state_of(*game);
        while (s.busy.exchange(true)) std::this_thread::sleep_for(std::chrono::milliseconds(50));
        run(*game, data, !test);
        s.busy = false;
        s.generation++;
    }
}
