// Builds and installs the Rush 2049 arcade mods (include/arcade2049.h) from the Games tab, on a worker thread: each
// mod is written as <mods>/<id>.rtz (a zip of what tools/arcade2049/track_mod.py and car_mod.py write), its mod.json
// carrying "rush2_arcade": the key of what it was built from (the input files, the shipped assets and this build)
// and the data folder. At launch, an installed mod whose key no longer matches (a new build, new arcade data) is
// built again once from that folder, keeping whether it was on. A loose mod folder of the same id (the Python tools'
// install) is replaced only by the button.

#include <atomic>
#include <chrono>
#include <fstream>
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

namespace a49 = rush2::arcade2049;

namespace {
    std::filesystem::path assets_dir() {
        return recompui::file::get_program_path() / "assets" / "arcade2049";
    }

    // The arcade folders each mod reads.
    std::vector<std::filesystem::path> inputs(const std::filesystem::path& data, bool cars) {
        if (!cars) return { data / "TRACKS" / "TRACK4" };
        std::vector<std::filesystem::path> out;
        for (const char* car : { "CAR2", "CAR4", "CAR5", "CAR6", "CAR7", "CAR8", "CAR10", "CAR11" }) out.push_back(data / "CARS" / car);
        return out;
    }

    // What a mod is built from: its input files' names and contents, the shipped assets and this build.
    std::string source_key(const std::filesystem::path& data, bool cars) {
        uint64_t h = rush2::track_cache::hash(nullptr, 0, rush2::track_cache::build() ^ (cars ? 0xCA45ull : 0x7AC4ull));
        std::vector<std::filesystem::path> files;
        std::error_code ec;
        for (const auto& dir : inputs(data, cars)) {
            for (auto it = std::filesystem::recursive_directory_iterator(dir, ec); !ec && it != std::filesystem::recursive_directory_iterator(); it.increment(ec)) {
                if (it->is_regular_file(ec)) files.push_back(it->path());
            }
        }
        if (!cars) files.push_back(assets_dir() / "alcatraz_logo.png"), files.push_back(assets_dir() / "alcatraz_preview.png");
        std::sort(files.begin(), files.end());
        for (const auto& f : files) {
            std::string name = rush2::upscale::path_utf8(f.lexically_relative(data));
            h = rush2::track_cache::hash(reinterpret_cast<const uint8_t*>(name.data()), name.size(), h);
            std::vector<uint8_t> bytes = rush2::upscale::read_file(f);
            h = rush2::track_cache::hash(bytes.data(), bytes.size(), h);
        }
        char text[17];
        snprintf(text, sizeof(text), "%016llx", (unsigned long long)h);
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
        // A loose folder of this id (the Python tools' install) would be a second mod of the same id.
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
            set_state(cars ? "Building the cars..." : "Building Alcatraz...");
            a49::ModFiles files;
            bool ok = cars ? a49::build_car_mod(data, files, log) : a49::build_track_mod(data, assets_dir(), files, log);
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
        }
        if (!refresh) set_state("Installed: Alcatraz and the arcade cars");
        else if (built) set_state("Rebuilt for this version");
    }

    bool start(const std::filesystem::path& data, bool refresh) {
        if (busy.exchange(true)) return false;
        // Detached: a build still running when the game closes leaves at most a .rtz.new behind.
        std::thread([data, refresh]() {
            run(data, refresh);
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

void a49::start_refresh() {
    static bool started = false;
    if (started) return;
    started = true;
    // Test aid: RUSH2_TEST_ARCADE_INSTALL=1 presses Build and Install at launch.
    if (const char* test = std::getenv("RUSH2_TEST_ARCADE_INSTALL"); test && *test == '1') {
        std::filesystem::path data = data_dir();
        if (!data.empty()) start(data, false);
        return;
    }
    // The check reads the installed mods and hashes their inputs: off the launcher's thread.
    std::thread([]() {
        std::filesystem::path data;
        for (const char* id : { track_mod_id, car_mod_id }) {
            Installed have = installed(id);
            if (have.rtz && !have.data.empty() && is_data_dir(have.data)) {
                data = have.data;
                break;
            }
        }
        if (!data.empty()) start(data, true);
    }).detach();
}
