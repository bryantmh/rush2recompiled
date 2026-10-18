// The asset catalog's sources and Dump Models (include/asset_catalog.h, docs/asset_catalog.md).
//
// Every model container the game can draw: Rush 2's assets as the game loads them now (the recomp's replacements
// included: the Rush 2049 cars at 0x71+, their part models, the menu containers with the SF Rush and 2049 logos), and
// every SF Rush and Rush 2049 track converted afresh with the converters the races use (a race only converts its own
// track). For the model dump, also every model file of the SF Rush and Rush 2049 ROMs converted on its own, so each
// model comes out under its own game's name. The converters are pure functions of the ROMs, so this runs on any
// thread.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <set>
#include <thread>

#include "librecomp/game.hpp"

#include "asset_catalog.h"
#include "assets.h"
#include "calspeed.h"
#include "car2049.h"
#include "rush2049_rom.h"
#include "track1.h"
#include "track2049.h"
#include "track2049_convert.h"
#include "wings.h"

namespace rush2::catalog {

namespace {
    // Rush 2's car types 0-21 (asset 0x1D + type) and track slots 0-11 (asset 0x33 + slot, the prefix table
    // 0x800C182C).
    constexpr const char* rush2_cars[] = { "PICKUP", "INTEG", "VETTE", "SLED", "BMW", "CAMARO", "SUPRA", "BUGAT", "VWBUS",
        "VIPER", "VWBUG", "CONCPT", "CIVIC", "CADDY", "MUST", "SUV", "TAXI", "HOTROD", "FORM1", "GT90", "ROCKET", "DEW" };
    constexpr const char* rush2_tracks[] = { "VEGAS", "NYONE", "HAWAII", "NYTWO", "ALCATRAZ", "LA", "SEATTLE", "HALFPIPE",
        "CRASH", "PIPE", "ATARI", "STUNT1" };
    constexpr int first_car = 0x1D, first_track = 0x33, first_placement = 0x3F;
    constexpr int first_2049_car = 0x71; // Rush 2049 car n (0-12) is asset 0x71 + n (src/rush2049/car2049.cpp).

    // Where Rush 2's asset `index` goes in the dumps (named after what they hold, docs/asset_catalog.md Sources).
    std::string asset_path(int index) {
        switch (index) {
            case 0x00: return "menus/attract";
            case 0x01: return "menus/background";
            case 0x02: return "menus/circuit";
            case 0x03: return "menus/track_select";
            case 0x11: return "menus/car_select";
            case 0x12: return "race/effects";
            case 0x13: return "hud/race";
            case 0x14: return "race/objects";
            case 0x15: return "menus/high_scores";
            case 0x16: return "hud/pause_1";
            case 0x17: return "hud/pause_2";
            case 0x18: return "hud/pause_3";
            case 0x19: return "menus/logos";
            case 0x1A: return "menus/title";
            case 0x1B: return "menus/medals";
            case 0x1C: return "cars/stripes";
            case rush2::car2049::parts_asset: return "cars/parts";
        }
        if (index >= 0x04 && index <= 0x10) {
            // The track maps: one per track slot (the prefix table's order), then the circuit's.
            return std::string("maps/") + (index == 0x10 ? "CIRCUIT" : rush2_tracks[index - 0x04]);
        }
        if (index >= first_car && index < first_car + 22) {
            return std::string("cars/") + rush2_cars[index - first_car];
        }
        if (index >= first_track && index < first_track + 12) {
            return std::string("tracks/") + rush2_tracks[index - first_track];
        }
        if (index >= first_2049_car && index < first_2049_car + rush2::car2049::car_count) {
            return std::string("cars/") + file_name(rush2::car2049::display_name(index - first_2049_car));
        }
        if (index >= rush2::car2049::first_calspeed_asset &&
            index < rush2::car2049::first_calspeed_asset + rush2::calspeed::car_count) {
            return std::string("cars/calspeed/") +
                   file_name(rush2::calspeed::car_names[index - rush2::car2049::first_calspeed_asset]);
        }
        char name[16];
        snprintf(name, sizeof(name), "assets/%02X", index);
        return name;
    }

    // SF Rush's model containers (docs/rush1_research.md section 1.1), as track1::convert_container converts them.
    constexpr int rush1_first_file = 7, rush1_last_file = 35, rush1_first_car = 25;
    constexpr const char* rush1_cars[] = { "BMW", "CAMARO", "SUPRA", "BUGATTI", "VWBUS", "VIPER", "VWBUG", "CONCEPT",
                                           "TAXI", "HOTROD", "FORM1" };
    std::string rush1_path(int index) {
        switch (index) {
            case 7: return "menus/title";
            case 8: return "menus/background";
            case 9: return "menus/main";
            case 10: return "menus/track_select";
            case 11: return "menus/car_select";
            case 12: return "race/objects";
            case 13: return "hud/race";
            case 14: return "menus/high_scores";
            case 15: case 16: case 17: return "hud/pause_" + std::to_string(index - 14);
            case 18: case 19: case 20: return "menus/labels_" + std::to_string(index - 17);
            case 21: case 22: case 23: return "menus/car_labels_" + std::to_string(index - 20);
            case 24: return "menus/trophies";
        }
        if (index >= rush1_first_car && index <= rush1_last_file) {
            return std::string("cars/") + rush1_cars[index - rush1_first_car];
        }
        return "assets/" + std::to_string(index);
    }

    // Rush 2049's model files (the ones holding OBHD objects) and where they go; a track's are its geometry (file
    // 100 + k) and, for a race track, its objects (81 + k), under track2049_path(k).
    struct File2049 {
        int file;
        const char* path;
    };
    constexpr File2049 files_2049[] = {
        { 54, "menus/interface" }, { 56, "cars/parts" }, { 57, "menus/trophies" }, { 60, "menus/track_select" },
        { 61, "race/explosions" }, { 62, "race/effects" }, { 63, "battle/coins" }, { 67, "stunt/icons" },
        { 68, "race/coins" }, { 76, "battle/weapons" }, { 77, "cars/wings" }, { 78, "race/flags" },
        { 79, "race/flag_triggers" }, { 80, "menus/wheel" }, { 81, "menus/wheel_small" },
    };

    // The name a converted Rush 2049 track goes under (convert_track's k).
    std::string track2049_path(int k) {
        using namespace rush2::track2049;
        if (k >= 1 && k <= track_count) return std::string("tracks/") + file_name(track_names[k - 1]);
        if (k >= battle_first && k < battle_first + battle_count) return "battle/DM" + std::to_string(k - battle_first + 1);
        if (k >= stunt_first && k < stunt_first + stunt_count) return "stunt/Arena_" + std::to_string(k - stunt_first + 1);
        if (k == obstacle) return "stunt/Obstacle_Course";
        return "tracks/" + std::to_string(k);
    }

    // A header that could be a model container (read_container checks the rest).
    bool maybe_container(const std::vector<uint8_t>& d) {
        if (d.size() < 0x28) return false;
        auto be = [&](size_t o) { return (uint32_t(d[o]) << 24) | (uint32_t(d[o + 1]) << 16) | (uint32_t(d[o + 2]) << 8) | d[o + 3]; };
        uint32_t models = be(16), textures = be(20);
        return (models > 0 || textures > 0) && models < 4096 && textures < 4096 && be(0) >= 0x28 && be(0) <= d.size() &&
               be(8) < d.size();
    }

    std::atomic<bool> model_dumping = false;
    std::mutex status_mutex;
    std::string model_status;

    void set_status(const std::string& s) {
        std::lock_guard lock{ status_mutex };
        model_status = s;
    }
}

rush2::origin::Game Entry::game_at(uint32_t offset) const {
    for (const Span& s : spans) {
        if (offset >= s.offset && offset - s.offset < s.size) {
            return s.game;
        }
    }
    return spans.empty() ? game : rush2::origin::Game::Rush2;
}

bool ui_path(const std::string& path) {
    for (const char* prefix : { "menus/", "hud/", "maps/", "stunt/icons" }) {
        if (path.rfind(prefix, 0) == 0) return true;
    }
    return false;
}

namespace {
    // The preview's paints: violet, yellow and cyan, hues no fixed entry of CARPALETTE has (its fixed colors are grays,
    // reds, oranges, greens and the glass's dark navy, which a blue MAIN's darker shades matched).
    constexpr uint8_t preview_paints[3][3] = { { 160, 80, 255 }, { 255, 215, 40 }, { 40, 220, 230 } };

    // The 2049 car (0-12) a container is, else -1.
    int car_2049_of(const Entry& e) {
        if (e.game != rush2::origin::Game::Rush2049) return -1;
        for (int n = 0; n < rush2::car2049::car_count; n++) {
            if (e.path == "cars/" + file_name(rush2::car2049::display_name(n))) return n;
        }
        return -1;
    }

    // 256 RGBA5551 entries (big-endian) as RGBA, opaque as the gray image is.
    std::vector<uint8_t> rgba_palette(const uint8_t* entries) {
        std::vector<uint8_t> palette(256 * 4);
        for (int k = 0; k < 256; k++) {
            uint16_t c = uint16_t((entries[k * 2] << 8) | entries[k * 2 + 1]);
            for (int ch = 0; ch < 3; ch++) {
                uint32_t v = (c >> (11 - ch * 5)) & 31;
                palette[k * 4 + ch] = uint8_t((v << 3) | (v >> 2));
            }
            palette[k * 4 + 3] = 255;
        }
        return palette;
    }
}

bool paint_base(const Entry& e, const Texture& t) {
    if (!t.painted || t.siz != 1 /* CI8 */ || t.indices.size() != size_t(t.width) * t.height ||
        e.path.rfind("cars/", 0) != 0) {
        return false;
    }
    rush2::origin::Game game = e.game_at(t.texels);
    return (game == rush2::origin::Game::Rush2 && e.path != "cars/stripes" && e.path != "cars/parts") ||
           (game == rush2::origin::Game::Rush2049 && car_2049_of(e) >= 0);
}

int car_2049(const Entry& e) {
    return car_2049_of(e);
}

int rush2_car(const Entry& e) {
    if (e.game != rush2::origin::Game::Rush2) return -1;
    for (int type = 0; type < int(std::size(rush2_cars)); type++) {
        if (e.path == std::string("cars/") + rush2_cars[type]) return type;
    }
    return -1;
}

std::vector<uint8_t> paint_preview_palette(const std::vector<Entry>& containers, const Entry& car) {
    auto be32 = [](const std::vector<uint8_t>& d, size_t o) {
        return uint32_t(d[o] << 24 | d[o + 1] << 16 | d[o + 2] << 8 | d[o + 3]);
    };
    if (int n = car_2049_of(car); n >= 0) {
        // The car's own palette with its ramps painted as rush2_car49_paint paints them: the paint, then each entry
        // 8/256 nearer black.
        std::vector<uint8_t> own = rush2::car2049::palette(n);
        if (own.size() != 512) return {};
        std::vector<uint8_t> palette = rgba_palette(own.data());
        const int first[3] = { 32, 64, 96 }, last[3] = { 63, 95, 126 };
        for (int r = 0; r < 3; r++) {
            for (int k = first[r]; k <= last[r]; k++) {
                int t = k == first[r] ? 0 : (k - first[r] - 1) * 8;
                for (int ch = 0; ch < 3; ch++) palette[k * 4 + ch] = uint8_t(preview_paints[r][ch] * (255 - t) >> 8);
            }
        }
        return palette;
    }
    for (const Entry& e : containers) {
        if (e.path != "cars/stripes" || !e.data) continue;
        const std::vector<uint8_t>& d = *e.data;
        if (d.size() < 0x28) return {};
        // The record table (as car_mods.cpp's load_car_palette reads it): 0x18-byte records, a 16-byte name first and
        // the data's offset at +20.
        uint32_t table = be32(d, 12), count = be32(d, 24);
        for (uint32_t i = 0; i < count && table + i * 0x18 + 0x18 <= d.size(); i++) {
            uint32_t r = table + i * 0x18;
            if (strncmp(reinterpret_cast<const char*>(&d[r]), "CARPALETTE", 16) != 0) continue;
            uint32_t data = be32(d, r + 20) & 0xFFFFFF;
            if (data + 512 > d.size()) return {};
            // MAIN and ACCENT: each entry the paint times CARPALETTE's gray over its lightest (230), as
            // car_build_textures_8008582C shades a paint.
            std::vector<uint8_t> palette = rgba_palette(&d[data]);
            for (int k = 1; k < 64; k++) {
                if (k == 32) continue;
                const uint8_t* paint = preview_paints[k < 32 ? 0 : 1];
                int level = palette[k * 4];
                for (int ch = 0; ch < 3; ch++) palette[k * 4 + ch] = uint8_t(std::min(255, paint[ch] * level / 230));
            }
            return palette;
        }
        return {};
    }
    return {};
}

Image paint_preview(const std::vector<uint8_t>& indices, uint32_t width, uint32_t height,
                    const std::vector<uint8_t>& palette) {
    Image out;
    if (palette.size() != 256 * 4 || indices.size() != size_t(width) * height) return out;
    out.width = width;
    out.height = height;
    out.rgba.resize(indices.size() * 4);
    for (size_t i = 0; i < indices.size(); i++) memcpy(&out.rgba[i * 4], &palette[indices[i] * 4], 4);
    return out;
}

std::vector<Entry> containers(const std::function<void(const std::string&)>& status, bool source_files) {
    std::vector<Entry> out;
    using rush2::origin::Game;

    // Rush 2's assets as loaded now. A track slot holding a converted track is left to the converted tracks below.
    status("Reading Rush 2...");
    std::set<std::string> shared_models;
    for (int i = 0; i < rush2::assets::slot_count; i++) {
        // A mod's cars (src/car_mods.cpp) are the mod's own files, not any game's.
        if (i >= rush2::car2049::first_mod_asset && i < rush2::car2049::first_mod_asset + rush2::car_types::count_mod) {
            continue;
        }
        std::vector<uint8_t> data;
        std::vector<rush2::assets::Span> spans;
        if (!rush2::assets::read_current(i, data, spans) || !maybe_container(data)) {
            continue;
        }
        // The track select's container gets the added tracks' art when that menu first opens: built here if need be,
        // so the catalog has it whichever menus have been opened.
        if (i == 3) {
            std::vector<uint8_t> menu;
            if (rush2::track2049::catalog_menu_container(menu)) {
                data = std::move(menu);
                spans.clear();
            }
        }
        bool converted_track = i >= first_track && i < first_track + 12 && !spans.empty();
        if (converted_track) {
            continue;
        }
        // For the model dump the 2049 cars and parts come from the 2049 files, under their own names.
        bool car_2049 = (i >= first_2049_car && i < first_2049_car + rush2::car2049::car_count) ||
                        i == rush2::car2049::parts_asset;
        if (source_files && car_2049) {
            continue;
        }
        if (i == 0x12 || i == 0x14) {
            std::set<std::string> names = rush2::track2049::model_names(data);
            shared_models.insert(names.begin(), names.end());
        }
        Entry e;
        e.game = Game::Rush2;
        e.path = asset_path(i);
        if (i >= first_2049_car && i < first_2049_car + rush2::car2049::car_count) {
            e.copies = rush2::car2049::scuffed_texels(i - first_2049_car);
        }
        // A replacement wholly from one game is that game's; mixed ones keep Rush 2 with their spans.
        size_t other = 0;
        for (const auto& s : spans) {
            if (s.game != Game::Rush2) other += s.size;
            e.spans.push_back({ s.offset, s.size, s.game });
        }
        if (!spans.empty() && other >= data.size()) {
            e.game = spans[0].game;
        }
        if (i >= first_track && i < first_track + 12) {
            std::vector<uint8_t> placement;
            std::vector<rush2::assets::Span> placement_spans;
            if (rush2::assets::read_current(first_placement + (i - first_track), placement, placement_spans)) {
                e.placement = std::make_shared<const std::vector<uint8_t>>(std::move(placement));
            }
        }
        e.data = std::make_shared<const std::vector<uint8_t>>(std::move(data));
        out.push_back(std::move(e));
    }

    // SF Rush's tracks, converted as a race would (the prefix only names the placement tree and FINISH model).
    if (auto rom = rush2::track1::get_rom()) {
        for (int t = 0; t < rush2::track1::track_count; t++) {
            status("Converting SF Rush " + std::to_string(t + 1) + "/" + std::to_string(rush2::track1::track_count) + "...");
            rush2::track1::ConvertedTrack track;
            std::string error;
            if (!rush2::track1::convert_track(*rom, t, "HAWAII", track, error)) {
                printf("[catalog] SF Rush track %d: %s\n", t + 1, error.c_str());
                continue;
            }
            Entry e;
            e.game = Game::SFRush;
            e.path = "tracks/" + file_name(rush2::track1::track_names[t]);
            e.data = std::make_shared<const std::vector<uint8_t>>(std::move(track.geometry));
            e.placement = std::make_shared<const std::vector<uint8_t>>(std::move(track.placement));
            out.push_back(std::move(e));
        }
        for (int i = rush1_first_file; source_files && i <= rush1_last_file; i++) {
            status("Converting SF Rush file " + std::to_string(i) + "...");
            std::vector<uint8_t> data;
            std::string error;
            if (!rush2::track1::convert_container(*rom, i, data, error)) {
                printf("[catalog] SF Rush file %d: %s\n", i, error.c_str());
                continue;
            }
            Entry e;
            e.game = Game::SFRush;
            e.path = rush1_path(i);
            e.data = std::make_shared<const std::vector<uint8_t>>(std::move(data));
            out.push_back(std::move(e));
        }
    }

    // Rush 2049's tracks and arenas, from the N64 ROM or the Dreamcast disc, whichever is the source.
    if (auto rom = rush2::wings::get_rom()) {
        Game game = rom->is_dreamcast() ? Game::Rush2049DC : Game::Rush2049;
        for (int k = 1; k <= rush2::track2049::obstacle; k++) {
            std::string path = track2049_path(k);
            status("Converting Rush 2049 " + std::to_string(k) + "/" + std::to_string(rush2::track2049::obstacle) + "...");
            rush2::track2049::ConvertedTrack track;
            std::string error;
            bool stunt = k >= rush2::track2049::battle_first;
            if (!rush2::track2049::convert_track(*rom, k, stunt ? "STUNT1" : "HAWAII", shared_models, true, track, error)) {
                printf("[catalog] Rush 2049 track %d: %s\n", k, error.c_str());
                continue;
            }
            Entry e;
            e.game = game;
            e.path = path;
            e.data = std::make_shared<const std::vector<uint8_t>>(std::move(track.geometry));
            e.placement = std::make_shared<const std::vector<uint8_t>>(std::move(track.placement));
            e.placed_only = source_files;
            out.push_back(std::move(e));
        }
        if (source_files) {
            std::vector<std::pair<int, std::string>> files;
            for (const File2049& f : files_2049) files.push_back({ f.file, f.path });
            // The cars as the game converts them (convert_car): 2049 copies a paint job's texels into the car file at
            // run time, so the file alone draws garbage where the paint goes. Their models take Rush 2's part names.
            for (int n = 0; n < rush2::car2049::car_count; n++) {
                status("Converting Rush 2049 car " + std::to_string(n + 1) + "...");
                std::vector<uint8_t> data;
                std::string error;
                if (!rush2::track2049::convert_car(*rom, n + 1, "CAR" + std::to_string(n + 1), data, error)) {
                    printf("[catalog] Rush 2049 car %d: %s\n", n + 1, error.c_str());
                    continue;
                }
                Entry e;
                e.game = game;
                e.path = "cars/" + file_name(rush2::car2049::display_name(n));
                e.data = std::make_shared<const std::vector<uint8_t>>(std::move(data));
                out.push_back(std::move(e));
            }
            for (int k = 1; k <= rush2::track2049::obstacle; k++) {
                files.push_back({ 100 + k, track2049_path(k) });
                if (k <= rush2::track2049::track_count) files.push_back({ 81 + k, track2049_path(k) + "/objects" });
            }
            for (size_t i = 0; i < files.size(); i++) {
                status("Converting Rush 2049 file " + std::to_string(i + 1) + "/" + std::to_string(files.size()) + "...");
                std::vector<uint8_t> data;
                std::string error;
                if (!rush2::track2049::convert_file(*rom, files[i].first, data, error)) {
                    printf("[catalog] Rush 2049 file %d: %s\n", files[i].first, error.c_str());
                    continue;
                }
                Entry e;
                e.game = game;
                e.path = files[i].second;
                e.data = std::make_shared<const std::vector<uint8_t>>(std::move(data));
                out.push_back(std::move(e));
            }
        }
    }
    return out;
}

std::filesystem::path model_dump_dir() {
    return rush2::upscale::asset_dump_dir() / "models";
}

bool model_dump_running() {
    return model_dumping;
}

std::string model_dump_status() {
    std::lock_guard lock{ status_mutex };
    return model_status;
}

namespace {
    // For tests: RUSH2_MODEL_DUMP=<seconds> presses Dump Models that long after the first asset load is possible.
    struct EnvTrigger {
        EnvTrigger() {
            if (const char* after = getenv("RUSH2_MODEL_DUMP")) {
                int seconds = atoi(after);
                std::thread([seconds]() {
                    std::this_thread::sleep_for(std::chrono::seconds(seconds));
                    start_model_dump();
                    while (model_dump_running()) std::this_thread::sleep_for(std::chrono::milliseconds(100));
                    printf("[catalog] %s\n", model_dump_status().c_str());
                    fflush(stdout);
                }).detach();
            }
        }
    } env_trigger;
}

bool start_model_dump() {
    if (model_dumping.exchange(true)) {
        return false;
    }
    set_status("Dumping...");
    std::thread([]() {
        std::vector<Entry> entries = containers(set_status, true);
        size_t written = 0, done = 0;
        // A fresh dump replaces the last one (the folder holds nothing but dumps; older ones had one file per container).
        for (rush2::origin::Game g : { rush2::origin::Game::Rush2, rush2::origin::Game::SFRush,
                                       rush2::origin::Game::Rush2049, rush2::origin::Game::Rush2049DC }) {
            std::error_code ec;
            std::filesystem::remove_all(model_dump_dir() / rush2::origin::folder(g), ec);
        }
        auto has_geometry = [](const Model& m) {
            for (const Lod& lod : m.lods) {
                for (const Mesh& mesh : lod.meshes) {
                    if (!mesh.vertices.empty()) return true;
                }
            }
            return false;
        };
        auto started = std::chrono::steady_clock::now();
        for (const Entry& e : entries) {
            set_status("Writing " + std::to_string(++done) + "/" + std::to_string(entries.size()) + "...");
            auto entry_started = std::chrono::steady_clock::now();
            struct SlowLog {
                const Entry& e;
                std::chrono::steady_clock::time_point at;
                ~SlowLog() {
                    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - at).count();
                    if (ms > 2000) printf("[catalog] %s/%s took %lld ms\n", rush2::origin::folder(e.game), e.path.c_str(), (long long)ms);
                }
            } slow_log{ e, entry_started };
            Container c;
            std::string error;
            // SF Rush's paint palettes are whole (BMWREDD0...); the 2049 cars' records cover part of a palette only.
            if (!read_container(*e.data, c, error, true, e.game == rush2::origin::Game::SFRush, &e.copies)) {
                continue;
            }
            std::vector<Placement> placements;
            if (e.placement != nullptr) {
                placements = read_placement(*e.placement);
            }
            std::filesystem::path dir = model_dump_dir() / rush2::origin::folder(e.game) / e.path;
            size_t models = 0;
            for (const Model& m : c.models) models += has_geometry(m);
            if (models == 0) {
                continue;
            }
            std::error_code ec;
            std::filesystem::create_directories(dir, ec);
            // The whole container too: the track as placed, or every model of a file with several (a car's parts).
            if (!placements.empty() || models > 1) {
                std::string title = e.path.substr(e.path.rfind('/') + 1);
                rush2::upscale::write_file(dir / (placements.empty() ? "_all.glb" : "_placed.glb"),
                                           write_glb(c, title, placements));
            }
            if (e.placed_only) {
                continue;
            }
            for (size_t m = 0; m < c.models.size(); m++) {
                if (!has_geometry(c.models[m])) continue;
                const std::string& name = c.models[m].name;
                if (rush2::upscale::write_file(dir / (file_name(name) + ".glb"), write_glb(single_model(c, m), name))) {
                    written++;
                }
            }
        }
        set_status("Dumped " + std::to_string(written) + " Models");
        printf("[catalog] Model dump: %lld s\n", (long long)std::chrono::duration_cast<std::chrono::seconds>(
                                                     std::chrono::steady_clock::now() - started).count());
        model_dumping = false;
    }).detach();
    return true;
}

}
