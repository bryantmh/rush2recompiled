// Builds and installs the PlayStation SF Rush mod (include/sfrush_ps1.h) from the Games tab, on a worker thread, as
// the arcade mods are (src/arcade2049/arcade_install.cpp): the mod is written as <mods>/sfrush_ps1.rtz, its mod.json
// carrying "rush2_ps1": the key of what it was built from (the disc's data track, the shipped logo and this build) and
// the disc's path. At launch, an installed mod whose key no longer matches (a new build, another disc) is built again
// once from that disc, keeping whether it was on.

#include <atomic>
#include <chrono>
#include <cstring>
#include <fstream>
#include <mutex>
#include <thread>

#include "json/json.hpp"
#include "librecomp/mods.hpp"
#include "recompui/recompui.h"
#include "util/file.h"
#include "elements/ui_button.h"
#include "elements/ui_label.h"

#include "sfrush_ps1.h"
#include "car_mods.h"
#include "options_page.h"
#include "texture_upscale.h"
#include "track1.h"
#include "track2049.h"
#include "track_cache.h"
#include "asset_catalog.h"

namespace ps1 = rush2::sfrush_ps1;

namespace {
    std::filesystem::path assets_dir() {
        return recompui::file::get_program_path() / "assets" / "sfrush_ps1";
    }

    // The N64 SF Rush's forward AI lanes of its tracks 0-2 when its ROM is in (the Games tab's SF Rush ROM), for the
    // PS1's versions of those courses (ps1_track.cpp); empty without it.
    ps1::N64Lanes n64_lanes() {
        ps1::N64Lanes out;
        auto rom = rush2::track1::get_rom();
        if (!rom) return out;
        for (int t = 0; t < 3; t++) {
            if (!rush2::track1::read_asset(*rom, 58 + t, out[size_t(t)])) out[size_t(t)].clear();
        }
        return out;
    }

    // The N64 SF Rush's Embarcadero (its track 1) as placed, its TRACK* models only (the course, not its props), for the
    // PS1 Embarcadero's overpass (ps1_track.cpp); empty without the ROM. The ROM's presence is in the key through
    // n64_lanes already, and the ROM is checked by its hash, so the scene needs no key of its own.
    ps1::N64Scene n64_embarcadero() {
        ps1::N64Scene out;
        auto rom = rush2::track1::get_rom();
        if (!rom) return out;
        rush2::track1::ConvertedTrack track;
        std::string error;
        rush2::catalog::Container c;
        if (!rush2::track1::convert_track(*rom, 1, "HAWAII", track, error) ||
            !rush2::catalog::read_container(track.geometry, c, error, true)) {
            printf("[ps1] the N64 Embarcadero couldn't be read: %s\n", error.c_str());
            return out;
        }
        std::vector<int> image_of(c.textures.size(), -1);
        auto wrap = [](rush2::upscale::Edge e) { return e == rush2::upscale::Edge::Clamp ? 33071 : e == rush2::upscale::Edge::Mirror ? 33648 : 10497; };
        rush2::catalog::for_each_placed_triangle({ &c }, rush2::catalog::read_placement(track.placement),
            [&](const rush2::catalog::PlacedTriangle& t) {
                if (t.model == nullptr || t.model->name.rfind("TRACK", 0) != 0 || t.mesh == nullptr) return;
                ps1::N64Scene::Tri tri{};
                for (int v = 0; v < 3; v++) {
                    // Front faces as drawn: a mirroring placement winds them the other way.
                    int s = t.mirrored && v > 0 ? 3 - v : v;
                    std::memcpy(tri.p[v], t.v[s], sizeof(tri.p[v]));
                    std::memcpy(tri.uv[v], t.source[s]->uv, sizeof(tri.uv[v]));
                    std::memcpy(tri.color[v], t.source[s]->color, sizeof(tri.color[v]));
                }
                int ti = t.mesh->texture;
                if (ti >= 0 && size_t(ti) < c.textures.size() && !c.textures[size_t(ti)].image.empty()) {
                    if (image_of[size_t(ti)] < 0) {
                        const auto& tex = c.textures[size_t(ti)];
                        image_of[size_t(ti)] = int(out.images.size());
                        out.images.push_back({ tex.image.width, tex.image.height, tex.image.rgba, wrap(tex.edge_s), wrap(tex.edge_t) });
                    }
                    tri.image = image_of[size_t(ti)];
                }
                out.tris.push_back(tri);
            });
        return out;
    }

    // What the mod is built from: the disc's data track (its name, size and time; reading 50 MB at every launch to
    // hash it isn't worth it), the shipped logo and diorama layout, the N64's lanes (a ROM added later rebuilds it)
    // and this build.
    std::string source_key(const std::filesystem::path& disc_path, const ps1::N64Lanes& n64) {
        ps1::Disc disc;
        std::string error;
        if (!disc.open(disc_path, error)) return "";
        std::error_code ec;
        uint64_t h = rush2::track_cache::hash(nullptr, 0, rush2::track_cache::build() ^ 0x5A51ull);
        std::string name = rush2::upscale::path_utf8(disc.data_track().filename());
        h = rush2::track_cache::hash(reinterpret_cast<const uint8_t*>(name.data()), name.size(), h);
        uint64_t size_time[2] = { (uint64_t)std::filesystem::file_size(disc.data_track(), ec),
                                  (uint64_t)std::filesystem::last_write_time(disc.data_track(), ec).time_since_epoch().count() };
        h = rush2::track_cache::hash(reinterpret_cast<const uint8_t*>(size_time), sizeof(size_time), h);
        for (const char* track : { "chinatown", "golden_gate", "embarcadero", "market" }) {
            std::vector<uint8_t> logo = rush2::upscale::read_file(assets_dir() / (std::string(track) + "_logo.png"));
            h = rush2::track_cache::hash(logo.data(), logo.size(), h);
        }
        std::vector<uint8_t> diorama = rush2::upscale::read_file(assets_dir() / "chinatown_diorama.json");
        h = rush2::track_cache::hash(diorama.data(), diorama.size(), h);
        for (const auto& lanes : n64) h = rush2::track_cache::hash(lanes.data(), lanes.size(), h ^ lanes.size());
        char text[17];
        snprintf(text, sizeof(text), "%016llx", (unsigned long long)h);
        return text;
    }

    struct Installed {
        bool rtz = false;
        std::string key;
        std::filesystem::path disc;
    };

    Installed installed() {
        Installed out;
        std::vector<uint8_t> manifest;
        rush2::upscale::ZipReader zip(recomp::mods::get_mods_directory() / (std::string(ps1::mod_id) + ".rtz"));
        if (zip.is_open() && zip.read("mod.json", manifest)) {
            out.rtz = true;
            nlohmann::json j = nlohmann::json::parse(manifest.begin(), manifest.end(), nullptr, false);
            if (j.is_object() && j.contains("rush2_ps1") && j["rush2_ps1"].is_object()) {
                out.key = j["rush2_ps1"].value("key", "");
                out.disc = std::filesystem::u8path(j["rush2_ps1"].value("disc", ""));
            }
        }
        return out;
    }

    std::mutex state_mutex;
    std::string state_text;                  // what the section shows; written by the worker
    std::atomic<int> generation = 0;         // counts changes, so the page reads the note again only then
    std::atomic<bool> busy = false;
    std::atomic<bool> rebuilding = false;    // the mod has been built this session (ps1::rebuilt)
    std::filesystem::path chosen_disc;       // the disc Select Disc picked, this session

    void set_state(const std::string& text) {
        std::lock_guard lock{ state_mutex };
        state_text = text;
        generation++;
    }

    std::string state() {
        std::lock_guard lock{ state_mutex };
        return state_text;
    }

    // Writes the mod as <mods>/sfrush_ps1.rtz in place of any copy, then has the game read it again. enable: turn it on
    // (the buttons); otherwise it keeps whether it was on.
    bool install(ps1::ModFiles& files, const std::string& key, const std::filesystem::path& disc, bool enable, std::string& error) {
        nlohmann::json manifest = nlohmann::json::parse(files["mod.json"].begin(), files["mod.json"].end());
        manifest["rush2_ps1"] = { { "key", key }, { "disc", rush2::upscale::path_utf8(disc) } };
        std::string text = manifest.dump(4);
        files["mod.json"].assign(text.begin(), text.end());
        std::filesystem::path mods = recomp::mods::get_mods_directory();
        std::error_code ec;
        std::filesystem::create_directories(mods, ec);
        std::filesystem::path pack = mods / (std::string(ps1::mod_id) + ".rtz"), fresh = mods / (std::string(ps1::mod_id) + ".rtz.new");
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
        // A first install goes to the end of the mod order: new mods are put first, and new cars take the 16 mod car
        // slots in mod order, so the PS1's cars would move the cars of mods already installed (the Rush 2049 arcade
        // cars) to other slots, and saves that picked them would get other cars.
        bool first_install = !installed().rtz;
        recomp::mods::close_mods();
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
        if (ok && first_install) recomp::mods::set_mod_index("rush2", ps1::mod_id, SIZE_MAX);
        if (ok && enable) recomp::mods::enable_mod(ps1::mod_id, true);
        recompui::update_mod_list(false);
        return ok;
    }

    // Builds and installs the mod from the disc. refresh: only when the installed one's key changed.
    void run(const std::filesystem::path& disc_path, bool refresh) {
        ps1::N64Lanes n64 = n64_lanes();
        std::string key = source_key(disc_path, n64);
        if (key.empty()) {
            set_state("Failed: the disc couldn't be read");
            return;
        }
        if (refresh) {
            Installed have = installed();
            if (!have.rtz || have.key.empty() || have.key == key) return;
        }
        rebuilding = true;
        set_state("Building the track and cars...");
        ps1::Disc disc;
        std::string error, log;
        ps1::ModFiles files;
        ps1::N64Scene embarcadero = n64_embarcadero();
        bool ok = disc.open(disc_path, error) && ps1::build_mod(disc, assets_dir(), files, log, n64, embarcadero);
        if (!ok || !install(files, key, disc_path, !refresh, error)) {
            if (error.empty()) error = "the disc's files couldn't be converted";
            printf("[ps1] %s\n%s", error.c_str(), log.c_str());
            fflush(stdout);
            set_state("Failed: " + error);
            return;
        }
        printf("[ps1] installed\n%s", log.c_str());
        fflush(stdout);
        rush2::car_mods::reload();
        rush2::track2049::mods_changed();
        set_state(refresh ? "Rebuilt" : "Installed: the PlayStation track and cars");
    }

    // A press while a build runs does nothing.
    bool start(const std::filesystem::path& disc) {
        if (busy.exchange(true)) return false;
        // Detached: a build still running when the game closes leaves at most a .rtz.new behind.
        std::thread([disc]() {
            run(disc, false);
            busy = false;
            generation++;
        }).detach();
        return true;
    }

    // The disc to build from: the one picked this session, else the installed mod's.
    std::filesystem::path disc_path() {
        std::filesystem::path chosen;
        {
            std::lock_guard lock{ state_mutex };
            chosen = chosen_disc;
        }
        if (!chosen.empty() && ps1::is_disc(chosen)) return chosen;
        Installed have = installed();
        if (!have.disc.empty() && ps1::is_disc(have.disc)) return have.disc;
        return {};
    }

    std::string status_note() {
        if (busy) return state();
        std::string s = state();
        if (!s.empty()) return s;
        Installed have = installed();
        if (!have.rtz) return "Needs the disc (see the description).";
        return disc_path().empty() ? "Installed. The disc isn't where it was." : "Installed.";
    }

    const char* description =
        "Adds what only the PlayStation version of San Francisco Rush has: its fourth track, Chinatown (listed after the "
        "SF Rush tracks), and seven cars: Starsky, Jeep, Jet Car, Mini, City Bus, Police and the UFO. The N64 version's "
        "tracks and cars stay as they are; this adds to them, and works without the N64 ROM too.<br/><br/>"
        "Press <recomp-color primary>Select Disc</recomp-color> and pick the disc image of San Francisco Rush - Extreme "
        "Racing (USA): its .cue, .chd or .ccd, or the first track's .bin, .img or .iso (an .iso has no music). It installs as a mod, SF Rush PlayStation, which the "
        "Mods menu can turn off. Building takes a second and you can keep playing; the track and cars show up the next "
        "time you're in the menus.";
}

void ps1::add_games_section(rush2::ui::OptionsPage* page, std::function<void()>& refresh) {
    using namespace recompui;
    ContextId context = get_current_context();
    rush2::ui::OptionsPage::Heading heading = page->add_heading("SF Rush PlayStation", status_note(), description);
    Element* row = page->add_row(description);
    // Both buttons on one line at full size: the Games tab's column is a few pixels short of them at add_row's 16 gap.
    row->set_gap(12.0f);
    Button* select = context.create_element<Button>(row, "Select Disc", ButtonStyle::Secondary);
    select->add_pressed_callback([]() {
        recompui::file::open_file_dialog([](bool success, const std::filesystem::path& path) {
            if (!success) return;
            if (!is_disc(path)) {
                recompui::message_box("That isn't the PlayStation San Francisco Rush - Extreme Racing (USA) disc. Pick its "
                                      ".cue, .chd or .ccd file, or the .bin, .img or .iso of its first track.");
                return;
            }
            {
                std::lock_guard lock{ state_mutex };
                chosen_disc = path;
            }
            start(path);
        });
    });
    Button* build = context.create_element<Button>(row, "Build and Install", ButtonStyle::Secondary);
    build->add_pressed_callback([]() {
        std::filesystem::path disc = disc_path();
        if (disc.empty()) {
            recompui::message_box("Press Select Disc and pick the PlayStation San Francisco Rush disc's .cue, .chd or .ccd "
                                  "file (or the .bin, .img or .iso of its first track).");
            return;
        }
        start(disc);
    });
    refresh = [note = heading.note, seen = generation.load()]() mutable {
        int now = generation.load();
        if (now == seen) return;
        seen = now;
        note->set_text(status_note());
    };
}

bool ps1::rebuilt() {
    return rebuilding;
}

std::filesystem::path ps1::disc() {
    std::filesystem::path d = disc_path();
    // Test aid: SFRUSH_PS1_DISC (as tools/sfrush_ps1 read it) when the installed mod's disc isn't where it was.
    const char* env = std::getenv("SFRUSH_PS1_DISC");
    if (d.empty() && env != nullptr && *env && ps1::is_disc(std::filesystem::u8path(env))) {
        d = std::filesystem::u8path(env);
    }
    return d;
}

void ps1::refresh_now() {
    static bool done = false;
    if (done) return;
    done = true;
    Installed have = installed();
    // Test aid: RUSH2_TEST_PS1_INSTALL=<disc> builds and installs from that disc at launch.
    const char* test = std::getenv("RUSH2_TEST_PS1_INSTALL");
    std::filesystem::path disc = test && *test ? std::filesystem::u8path(test) : have.rtz && !have.key.empty() ? disc_path()
                                                                                                               : std::filesystem::path();
    if (disc.empty() || !is_disc(disc)) return;
    while (busy.exchange(true)) std::this_thread::sleep_for(std::chrono::milliseconds(50));
    run(disc, !(test && *test));
    busy = false;
    generation++;
}
