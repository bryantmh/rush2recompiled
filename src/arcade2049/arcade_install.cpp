// Builds and installs the Rush 2049 arcade mods (include/arcade2049.h) from the Games tab, on a worker thread: each
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
    std::filesystem::path assets_dir() {
        return recompui::file::get_program_path() / "assets" / "arcade2049";
    }

    // The Games tab's Alcatraz Only option: the track mod holds just Alcatraz, else every arcade track
    // (a49::arcade_tracks). Changing it rebuilds an installed mod in the background (start).
    const std::string alcatraz_only_option_id = "arcade_alcatraz_only";
    std::atomic_bool alcatraz_only = true;

    // The arcade folders each mod reads.
    std::vector<std::filesystem::path> inputs(const std::filesystem::path& data, bool cars) {
        std::vector<std::filesystem::path> out;
        if (!cars) {
            for (const a49::ArcadeTrack& a : a49::arcade_tracks) {
                if (!alcatraz_only || std::string(a.id) == "alcatraz") out.push_back(data / "TRACKS" / a.folder);
            }
            return out;
        }
        for (const char* car : { "CAR2", "CAR4", "CAR5", "CAR6", "CAR7", "CAR8", "CAR10", "CAR11" }) out.push_back(data / "CARS" / car);
        return out;
    }

    std::string compute_source_key(const std::filesystem::path& data, bool cars);

    // What a mod is built from: its input files' names and contents, the shipped assets and this build. Worked out once
    // per data folder, mod and Alcatraz Only setting (it reads every input file), for the refresh and a race's check.
    std::string source_key(const std::filesystem::path& data, bool cars) {
        static std::mutex memo_mutex;
        static std::map<std::string, std::string> memo;
        std::string id = rush2::upscale::path_utf8(data) + (cars ? "|cars" : alcatraz_only ? "|alcatraz" : "|tracks");
        std::lock_guard lock{ memo_mutex };
        auto it = memo.find(id);
        if (it != memo.end()) return it->second;
        return memo[id] = compute_source_key(data, cars);
    }

    std::string compute_source_key(const std::filesystem::path& data, bool cars) {
        uint64_t h = rush2::track_cache::hash(nullptr, 0, rush2::track_cache::build() ^ (cars ? 0xCA45ull : 0x7AC4ull));
        std::vector<std::filesystem::path> files;
        std::error_code ec;
        for (const auto& dir : inputs(data, cars)) {
            for (auto it = std::filesystem::recursive_directory_iterator(dir, ec); !ec && it != std::filesystem::recursive_directory_iterator(); it.increment(ec)) {
                if (it->is_regular_file(ec)) files.push_back(it->path());
            }
        }
        if (!cars) {
            for (const a49::ArcadeTrack& a : a49::arcade_tracks) {
                if (alcatraz_only && std::string(a.id) != "alcatraz") continue;
                for (const char* file : { "_logo.png", "_preview.png" }) files.push_back(assets_dir() / (std::string(a.id) + file));
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
        const std::string blob = std::string("arcade_source_key_") + (cars ? "cars" : alcatraz_only ? "alcatraz" : "tracks");
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

    std::mutex state_mutex;
    std::string state_text;                  // what the section shows; written by the worker
    std::atomic<int> state_generation = 0;   // counts changes, so the page reads the note again only then
    std::atomic<bool> busy = false;
    std::atomic<bool> rebuilding = false;    // a mod has been built this session (a49::rebuilt)
    std::filesystem::path chosen_data;       // the folder Select Folder picked, this session

    void set_state(const std::string& text) {
        std::lock_guard lock{ state_mutex };
        state_text = text;
        state_generation++;
    }

    std::string state() {
        std::lock_guard lock{ state_mutex };
        return state_text;
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

    // Builds and installs both mods from `data`. refresh: only those installed by the game whose key changed.
    void run(std::filesystem::path data, bool refresh) {
        std::string error, log;
        int built = 0;
        for (bool cars : { false, true }) {
            const std::string id = cars ? a49::car_mod_id : a49::track_mod_id;
            std::string key = source_key(data, cars);
            if (refresh) {
                Installed have = installed(id);
                if (!have.rtz || have.key.empty() || have.key == key) continue;
            }
            rebuilding = true;
            set_state(cars ? "Building the cars..." : alcatraz_only ? "Building Alcatraz..." : "Building the arcade tracks...");
            a49::ModFiles files;
            bool ok = cars ? a49::build_car_mod(data, files, log) : a49::build_track_mod(data, assets_dir(), files, log, alcatraz_only);
            if (!ok || !install(id, files, key, data, !refresh, error)) {
                if (error.empty()) error = "the arcade data couldn't be read";
                printf("[arcade2049] %s: %s\n%s", id.c_str(), error.c_str(), log.c_str());
                set_state("Failed: " + error);
                return;
            }
            built++;
        }
        printf("[arcade2049] %s\n%s", built ? "installed" : "up to date", log.c_str());
        if (built) {
            rush2::car_mods::reload();
            rush2::track2049::mods_changed();
            a49::checkpoints_changed();
        }
        if (!refresh) set_state(alcatraz_only ? "Installed: Alcatraz and the arcade cars" : "Installed: the arcade tracks and cars");
        else if (built) set_state("Rebuilt");
    }

    std::atomic_bool refresh_again = false;   // the Alcatraz Only option changed during a build

    bool start(const std::filesystem::path& data, bool refresh) {
        if (busy.exchange(true)) {
            if (refresh) refresh_again = true;
            return false;
        }
        // Detached: a build still running when the game closes leaves at most a .rtz.new behind.
        std::thread([data, refresh]() {
            run(data, refresh);
            while (refresh_again.exchange(false)) run(data, true);
            busy = false;
            state_generation++;
        }).detach();
        return true;
    }

    // The data folder to build from: the one picked this session, else the installed mods', else ProjectR's.
    std::filesystem::path data_dir() {
        if (!chosen_data.empty() && a49::is_data_dir(chosen_data)) return chosen_data;
        for (const char* id : { a49::track_mod_id, a49::car_mod_id }) {
            Installed have = installed(id);
            if (!have.data.empty() && a49::is_data_dir(have.data)) return have.data;
        }
        std::filesystem::path d = a49::default_data_dir();
        return a49::is_data_dir(d) ? d : std::filesystem::path();
    }

    std::string status_note() {
        if (busy) return state();
        std::string s = state();
        if (!s.empty()) return s;
        bool track = installed(a49::track_mod_id).rtz || installed(a49::track_mod_id).loose;
        if (data_dir().empty()) return track ? "Installed. Arcade data not found." : "Needs the arcade data (see the description).";
        return track ? "Installed." : "Arcade data found.";
    }
}

void a49::add_games_section(rush2::ui::OptionsPage* page, std::function<void()>& refresh) {
    using namespace recompui;
    ContextId context = get_current_context();
    const std::string description =
        "Adds Alcatraz, the track only the Rush 2049 arcade game has (listed after the Rush 2049 tracks), and the "
        "arcade's Truck, Deuce and Caddy with their paint styles, plus arcade paint styles for the GX-2, Magnum, "
        "Super GT, 8-Ball and Bruiser (the car select's STYLE row).<br/><br/>"
        "They're built from the arcade game's files, which ProjectR unpacks from a Rush 2049 Special Edition arcade "
        "hard drive. If ProjectR has done that on this computer, <recomp-color primary>Build and Install</recomp-color> "
        "finds them; otherwise press <recomp-color primary>Select Folder</recomp-color> and pick any file in the "
        "unpacked folder (sf2049se).<br/><br/>"
        "They install as two mods, Rush 2049 Arcade Tracks and Rush 2049 Arcade Cars, which the Mods menu can turn off. "
        "Building takes a few seconds and you can keep playing; they show up the next time you're in the menus.";
    rush2::ui::OptionsPage::Heading heading = page->add_heading("Rush 2049 Arcade", status_note(), description);
    Element* row = page->add_row(description);
    Button* build = context.create_element<Button>(row, "Build and Install", ButtonStyle::Secondary);
    build->add_pressed_callback([]() {
        std::filesystem::path data = data_dir();
        if (data.empty()) {
            message_box("The Rush 2049 arcade files weren't found. Press Select Folder and pick any file in the folder "
                        "ProjectR unpacked them to (sf2049se).");
            return;
        }
        start(data, false);
    });
    Button* select = context.create_element<Button>(row, "Select Folder", ButtonStyle::Secondary);
    select->add_pressed_callback([]() {
        recompui::file::open_file_dialog([](bool success, const std::filesystem::path& path) {
            if (!success) return;
            std::optional<std::filesystem::path> data = find_data_dir(path);
            if (!data) {
                recompui::message_box("That isn't in the Rush 2049 arcade files. Pick a file in the folder ProjectR "
                                      "unpacked them to (sf2049se, with TRACKS and CARS inside).");
                return;
            }
            chosen_data = *data;
            start(*data, false);
        });
    });
    refresh = [note = heading.note, seen = state_generation.load()]() mutable {
        int now = state_generation.load();
        if (now == seen) return;
        seen = now;
        note->set_text(status_note());
    };
}

void a49::init_config() {
    recomp::config::Config& config = rush2::wings::games_config();
    config.add_bool_option(
        alcatraz_only_option_id,
        "Alcatraz Only",
        "On, the arcade tracks are just Alcatraz, the track only the arcade game has. Off, the arcade game's own "
        "versions of Marina, Haight, Civic, Metro, Mission and Presidio are added too, as tracks of their own after the "
        "Rush 2049 tracks. Changing it rebuilds the arcade tracks in the background (it needs the arcade data); they "
        "show up the next time you're in the menus.",
        true
    );
    config.add_option_change_callback(alcatraz_only_option_id,
        [](recomp::config::ConfigValueVariant cur_value, recomp::config::ConfigValueVariant, recomp::config::OptionChangeContext context) {
            bool on = std::get<bool>(cur_value);
            // Loading the settings: the launch's refresh (refresh_now) builds an out of date mod before the game starts.
            if (on == alcatraz_only.exchange(on) || context == recomp::config::OptionChangeContext::Load) return;
            // Only an installed mod is rebuilt (run's refresh compares its key, which holds the option).
            std::filesystem::path data = data_dir();
            if (!data.empty()) start(data, true);
        });
    init_checkpoints_config(config);
}

void a49::load_config() {
    alcatraz_only = std::get<bool>(rush2::wings::games_config().get_option_value(alcatraz_only_option_id));
    load_checkpoints_config();
}

void a49::add_options(rush2::ui::OptionsPage* page) {
    page->add_option(rush2::wings::games_config(), alcatraz_only_option_id);
    add_checkpoints_option(page);
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
    // A game-built mod is rebuilt from the folder it was built from, or wherever data_dir finds the arcade files if
    // that one moved.
    bool built_by_game = false;
    for (const char* id : { track_mod_id, car_mod_id }) {
        Installed have = installed(id);
        built_by_game = built_by_game || (have.rtz && !have.key.empty());
    }
    std::filesystem::path data = built_by_game || test ? data_dir() : std::filesystem::path();
    if (data.empty()) return;
    // A build already going (the Games tab's button) is waited for, then the check runs again on what it installed.
    while (busy.exchange(true)) std::this_thread::sleep_for(std::chrono::milliseconds(50));
    run(data, !test);
    while (refresh_again.exchange(false)) run(data, true);
    busy = false;
    state_generation++;
}
