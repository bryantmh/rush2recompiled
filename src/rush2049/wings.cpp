// Rush 2049 wings: settings (shown in the Games tab; the wing styles on the car select, src/rush2049/wings_menu.cpp) and Rush
// 2049 ROM handling.
//
// San Francisco Rush 2049 lets cars deploy wings while airborne. This port reads the wing models and sound from the
// user's own Rush 2049 ROM (NTSC-U), so nothing from that game ships with the recomp. The ROM is chosen with a button
// on the Games tab, checked, and copied in big-endian (.z64) byte order to the app folder as rush2049.z64. The
// Wings option stays disabled until a valid ROM is present.
//
// The ability itself lives in src/wings_*.cpp.

// librecomp is built against its own nlohmann::json (3.9). The executable's include path finds RT64's newer copy
// first, whose ABI-tagged namespace changes the mangled name of Config::load_config, so this file includes
// librecomp's copy before anything else (the header guard keeps it).
#include "../../lib/N64ModernRuntime/thirdparty/json/json.hpp"

#include <array>
#include <atomic>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "recompui/recompui.h"
#include "recompui/config.h"
#include "librecomp/config.hpp"
#include "librecomp/game.hpp"
#include "util/file.h"
#include "elements/ui_button.h"
#include "elements/ui_label.h"
#include "options_page.h"

#include "rush2.h"
#include "car2049.h"
#include "battle.h"
#include "track2049.h"
#include "wings_internal.h"
#include "rush2049_dc.h"
#include "texture_upscale.h"
#include "odometer2049.h"

namespace {
    const std::string config_id = "games";
    const std::string wings_option_id = "wings";
    const std::string tracks_option_id = "rush2049_tracks";
    const std::string cars_option_id = "rush2049_cars";
    const std::string drones_option_id = "rush2049_computer_cars";
    const std::string speeds_option_id = "car_speeds";
    const std::string odometer_option_id = "rush2049_odometer";
    const std::string source_option_id = "rush2049_source";
    const std::string battle_time_option_id = "battle_time_limit";
    const std::string battle_team_option_prefix = "battle_team_p";
    const std::string style_option_prefix = "wing_style_p"; // + the player (1-4): their wings, set on the car select

    std::string style_option(int player) {
        return style_option_prefix + std::to_string(player + 1);
    }
    const char* rom_file_name = "rush2049.z64";

    constexpr size_t rom_size = 0xC00000;
    // SHA-1 of San Francisco Rush 2049 (USA) in big-endian byte order.
    constexpr std::array<uint8_t, 20> rom_sha1 = {
        0x3f, 0x99, 0x35, 0x1d, 0x7b, 0xb6, 0x16, 0x56, 0x61, 0x4b,
        0xdb, 0x2a, 0xa1, 0xa9, 0x0c, 0xfe, 0x55, 0xd1, 0x92, 0x2c,
    };

    // The Games tab's config, games.json: the Rush 2049 options here and SF Rush's (src/rush1/rush1_rom.cpp, through
    // games_config()). Owned here instead of through create_config_tab: its options are shown in the Games tab
    // (src/games_tab.cpp) under the ROM pickers, and the wing styles in the Players tab.
    recomp::config::Config wings_config{ "Games", config_id, false };

    std::mutex rom_mutex;
    std::shared_ptr<const rush2::rom2049::Source> rom_data; // Null until a valid ROM or disc is loaded.
    std::atomic_bool importing = false; // A Dreamcast disc is being read into the app folder.
    std::atomic_bool wings_option = false;
    std::atomic_bool config_loaded = false; // load_config has run: a source change loads the other game from disk.

    // Which Rush 2049 the Source option asks for when both the N64 ROM and the Dreamcast disc are in the app folder.
    enum class SourceChoice : uint32_t { N64 = 0, Dreamcast = 1 };

    // Converts .v64/.n64 (byteswapped) and little-endian images to big-endian. Returns false if it isn't an N64 ROM.
    bool to_big_endian(std::vector<uint8_t>& data) {
        if (data.size() < 0x40 || data.size() % 4 != 0) {
            return false;
        }
        if (data[0] == 0x80 && data[1] == 0x37 && data[2] == 0x12 && data[3] == 0x40) {
            return true;
        }
        if (data[0] == 0x37 && data[1] == 0x80 && data[2] == 0x40 && data[3] == 0x12) {
            for (size_t i = 0; i < data.size(); i += 2) {
                std::swap(data[i], data[i + 1]);
            }
            return true;
        }
        if (data[0] == 0x40 && data[1] == 0x12 && data[2] == 0x37 && data[3] == 0x80) {
            for (size_t i = 0; i < data.size(); i += 4) {
                std::swap(data[i], data[i + 3]);
                std::swap(data[i + 1], data[i + 2]);
            }
            return true;
        }
        return false;
    }

    enum class RomCheck { Good, FailedToOpen, NotARom, WrongGame, WrongVersion };

    RomCheck read_rom(const std::filesystem::path& path, std::vector<uint8_t>& out) {
        std::ifstream file{ path, std::ios::binary | std::ios::ate };
        if (!file) {
            return RomCheck::FailedToOpen;
        }
        std::streamsize size = file.tellg();
        if (size <= 0 || size > 0x4000000) {
            return RomCheck::NotARom;
        }
        out.resize(size_t(size));
        file.seekg(0);
        if (!file.read(reinterpret_cast<char*>(out.data()), size)) {
            return RomCheck::FailedToOpen;
        }
        if (!to_big_endian(out)) {
            return RomCheck::NotARom;
        }
        // Game code at 0x3B: "NRU" is Rush 2049 in any region; the last byte is the region.
        if (memcmp(&out[0x3B], "NRU", 3) != 0) {
            return RomCheck::WrongGame;
        }
        if (out.size() != rom_size || rush2::wings::rom_sha1(out) != rom_sha1) {
            return RomCheck::WrongVersion;
        }
        return RomCheck::Good;
    }

    std::filesystem::path stored_rom_path() {
        return recomp::get_config_path() / rom_file_name;
    }

    // The Dreamcast disc's files, as rush2::rom2049::dc::import writes them.
    std::filesystem::path stored_pack_path() {
        return recomp::get_config_path() / rush2::rom2049::dc::pack_file_name;
    }

    void set_rom(std::shared_ptr<const rush2::rom2049::Source> rom) {
        {
            std::lock_guard lock{ rom_mutex };
            rom_data = rom;
        }
        // A stored disc gives the N64 ROM its Dreamcast Textures too.
        std::error_code ec;
        bool disc_stored = std::filesystem::exists(stored_pack_path(), ec);
        rush2::rom2049::dc::set_texture_sources(rom, disc_stored ? stored_pack_path() : std::filesystem::path());
        rush2::upscale::set_dreamcast_option_state(rom != nullptr && rom->is_dreamcast(), disc_stored);
        rush2::wings::on_rom_changed();
    }

    std::string rom_status_text() {
        if (importing) {
            return "Reading the Dreamcast disc...";
        }
        auto rom = rush2::wings::get_rom();
        if (rom == nullptr) {
            return "Needs a San Francisco Rush 2049 (USA) N64 ROM or Dreamcast disc for its tracks, cars and wings.";
        }
        return rom->is_dreamcast() ? "Using the Dreamcast disc (experimental: some content untested)."
                                   : "Using the N64 ROM.";
    }

    bool both_sources_stored() {
        std::error_code ec;
        return std::filesystem::exists(stored_rom_path(), ec) && std::filesystem::exists(stored_pack_path(), ec);
    }

    std::shared_ptr<const rush2::rom2049::Source> open_stored_rom() {
        std::filesystem::path path = stored_rom_path();
        std::error_code ec;
        if (!std::filesystem::exists(path, ec)) {
            return nullptr;
        }
        std::vector<uint8_t> data;
        if (read_rom(path, data) != RomCheck::Good) {
            printf("[Wings] Ignoring %s: not a valid Rush 2049 (USA) ROM\n", path.string().c_str());
            return nullptr;
        }
        return rush2::rom2049::n64_source(std::make_shared<const std::vector<uint8_t>>(std::move(data)));
    }

    std::shared_ptr<const rush2::rom2049::Source> open_stored_pack() {
        std::filesystem::path pack = stored_pack_path();
        std::error_code ec;
        if (!std::filesystem::exists(pack, ec)) {
            return nullptr;
        }
        auto source = rush2::rom2049::dc::open_pack(pack);
        if (source == nullptr) {
            printf("[Wings] Ignoring %s: not a Rush 2049 Dreamcast pack this version can read\n", pack.string().c_str());
        }
        return source;
    }

    // Loads the stored game the Source option chooses, else the other one if only it is there.
    void load_chosen_source() {
        auto choice = static_cast<SourceChoice>(std::get<uint32_t>(wings_config.get_option_value(source_option_id)));
        std::shared_ptr<const rush2::rom2049::Source> source =
            choice == SourceChoice::N64 ? open_stored_rom() : open_stored_pack();
        if (source == nullptr) {
            source = choice == SourceChoice::N64 ? open_stored_pack() : open_stored_rom();
        }
        if (source != nullptr) {
            set_rom(source);
        }
    }

    // Points the Source option at the game just chosen. Its callback skips a source that's already loaded.
    void choose_source(SourceChoice choice) {
        wings_config.update_option_value(source_option_id, static_cast<uint32_t>(choice));
        wings_config.save_config();
    }

    void update_rom_ui() {
        bool disabled = !rush2::wings::rom_available();
        wings_config.update_option_disabled(source_option_id, importing || !both_sources_stored());
        wings_config.update_option_disabled(wings_option_id, disabled);
        wings_config.update_option_disabled(tracks_option_id, disabled);
        wings_config.update_option_disabled(cars_option_id, disabled);
        wings_config.update_option_disabled(odometer_option_id, disabled);
        // The computer cars' choice is among the Rush 2049 cars, and the battle options are the arenas', which come
        // with the Rush 2049 tracks.
        bool cars_on = std::get<bool>(wings_config.get_option_value(cars_option_id));
        wings_config.update_option_disabled(drones_option_id, disabled || !cars_on);
        bool battles = rush2::track2049::available();
        wings_config.update_option_disabled(battle_time_option_id, !battles);
        for (int player = 1; player <= 4; player++) {
            wings_config.update_option_disabled(battle_team_option_prefix + std::to_string(player), !battles);
        }
        for (int player = 0; player < rush2::wings::max_players; player++) {
            wings_config.update_option_disabled(style_option(player), disabled);
        }
    }

    void select_dreamcast_disc(const std::filesystem::path& path) {
        if (importing.exchange(true)) {
            return;
        }
        update_rom_ui();
        std::thread([path]() {
            using rush2::rom2049::dc::ImportResult;
            ImportResult result;
            try {
                result = rush2::rom2049::dc::import(path, stored_pack_path());
            }
            catch (const std::exception&) {
                // A damaged image (a garbled .gdi number, a huge file size in its directory): not a crash.
                result = ImportResult::FailedToOpen;
            }
            std::shared_ptr<const rush2::rom2049::Source> source;
            if (result == ImportResult::Good) {
                source = rush2::rom2049::dc::open_pack(stored_pack_path());
                if (source == nullptr) {
                    result = ImportResult::WriteFailed;
                }
            }
            importing = false;
            switch (result) {
                case ImportResult::Good:
                    // The disc takes over from any N64 ROM chosen before. The ROM's copy stays, so the Source
                    // option can switch back and the experimental disc support never costs anyone their setup.
                    set_rom(source);
                    choose_source(SourceChoice::Dreamcast);
                    break;
                case ImportResult::FailedToOpen:
                    recompui::message_box("Failed to open the disc image.");
                    break;
                case ImportResult::NotADisc:
                    recompui::message_box("This is not a Dreamcast disc image (.cdi, .gdi or .iso).");
                    break;
                case ImportResult::WrongGame:
                    recompui::message_box("This disc is not San Francisco Rush 2049.");
                    break;
                case ImportResult::WrongVersion:
                    recompui::message_box("This disc is San Francisco Rush 2049, but the wrong version.\n"
                        "The Dreamcast version must be the NTSC-U (USA) one.");
                    break;
                case ImportResult::WriteFailed:
                    recompui::message_box("Failed to copy the Rush 2049 disc's files into the app folder.");
                    break;
            }
            update_rom_ui();
        }).detach();
    }

    void select_rom() {
        recompui::file::open_file_dialog([](bool success, const std::filesystem::path& path) {
            if (!success) {
                return;
            }
            if (rush2::rom2049::dc::is_disc_image(path)) {
                select_dreamcast_disc(path);
                return;
            }
            auto data = std::make_shared<std::vector<uint8_t>>();
            switch (read_rom(path, *data)) {
                case RomCheck::Good:
                    break;
                case RomCheck::FailedToOpen:
                    recompui::message_box("Failed to open ROM file.");
                    return;
                case RomCheck::NotARom:
                    recompui::message_box("This is not a valid ROM file.");
                    return;
                case RomCheck::WrongGame:
                    recompui::message_box("This ROM is not San Francisco Rush 2049.");
                    return;
                case RomCheck::WrongVersion:
                    recompui::message_box("This ROM is San Francisco Rush 2049, but the wrong version.\n"
                        "Rush 2049 content requires the NTSC-U (USA) N64 version.");
                    return;
            }

            std::error_code ec;
            std::filesystem::create_directories(recomp::get_config_path(), ec);
            std::ofstream out{ stored_rom_path(), std::ios::binary };
            if (!out.write(reinterpret_cast<const char*>(data->data()), data->size())) {
                recompui::message_box("Failed to copy the Rush 2049 ROM into the app folder.");
                return;
            }
            out.close();
            // The ROM takes over from any Dreamcast disc chosen before; the disc's pack stays for the Source option.
            set_rom(rush2::rom2049::n64_source(data));
            choose_source(SourceChoice::N64);
            update_rom_ui();
        });
    }
}

recomp::config::Config& rush2::wings::games_config() {
    return wings_config;
}

std::shared_ptr<const rush2::rom2049::Source> rush2::wings::get_rom() {
    std::lock_guard lock{ rom_mutex };
    return rom_data;
}

bool rush2::wings::rom_available() {
    std::lock_guard lock{ rom_mutex };
    return rom_data != nullptr;
}

bool rush2::wings::enabled() {
    return wings_option.load(std::memory_order_relaxed) && rom_available();
}

void rush2::wings::init_config() {
    wings_config.add_enum_option(
        source_option_id,
        "Rush 2049 Source",
        "Which copy of Rush 2049 to read when both its N64 ROM and its Dreamcast disc have been selected. "
        "<recomp-color primary>Dreamcast Disc</recomp-color> has larger textures and the disc's own sound and music "
        "(experimental). Takes effect at the next race or car select.",
        {
            { SourceChoice::N64, "N64", "N64 ROM" },
            { SourceChoice::Dreamcast, "Dreamcast", "Dreamcast Disc" },
        },
        // A disc chosen before this option existed was the one in use (the pack won), so it stays the default.
        SourceChoice::Dreamcast
    );
    wings_config.add_option_change_callback(source_option_id,
        [](recomp::config::ConfigValueVariant cur_value, recomp::config::ConfigValueVariant, recomp::config::OptionChangeContext) {
            if (!config_loaded || importing) {
                return;
            }
            auto rom = rush2::wings::get_rom();
            bool want_dc = static_cast<SourceChoice>(std::get<uint32_t>(cur_value)) == SourceChoice::Dreamcast;
            if (rom != nullptr && rom->is_dreamcast() == want_dc) {
                return;
            }
            load_chosen_source();
            update_rom_ui();
        });

    wings_config.add_bool_option(
        wings_option_id,
        "Wings",
        "Adds the wings from San Francisco Rush 2049. Hold the <recomp-color primary>Wings</recomp-color> button "
        "(set it in the game's Controller Setup screen) while airborne to spread them, then steer in the air with "
        "the stick. Requires a Rush 2049 (USA) ROM.",
        false
    );
    wings_config.add_option_change_callback(wings_option_id,
        [](recomp::config::ConfigValueVariant cur_value, recomp::config::ConfigValueVariant, recomp::config::OptionChangeContext) {
            wings_option = std::get<bool>(cur_value);
        });

    wings_config.add_bool_option(
        tracks_option_id,
        "Rush 2049 Tracks",
        "Adds the six race tracks of San Francisco Rush 2049 to the track select, after Rush 2's own tracks. "
        "Requires a Rush 2049 (USA) ROM.",
        true
    );
    wings_config.add_option_change_callback(tracks_option_id,
        [](recomp::config::ConfigValueVariant cur_value, recomp::config::ConfigValueVariant, recomp::config::OptionChangeContext) {
            rush2::track2049::set_option(std::get<bool>(cur_value));
            update_rom_ui();
        });
    wings_config.add_bool_option(
        cars_option_id,
        "Rush 2049 Cars",
        "Adds the thirteen cars of San Francisco Rush 2049 to the car select, after Rush 2's own cars, for players "
        "and the computer cars. Takes effect the next time the car select opens. Requires a Rush 2049 (USA) ROM.",
        true
    );
    wings_config.add_option_change_callback(cars_option_id,
        [](recomp::config::ConfigValueVariant cur_value, recomp::config::ConfigValueVariant, recomp::config::OptionChangeContext) {
            rush2::car2049::set_option(std::get<bool>(cur_value));
            update_rom_ui();
        });

    wings_config.add_enum_option(
        drones_option_id,
        "Rush 2049 Computer Cars",
        "Sets where the computer cars drive Rush 2049 cars (with Rush 2049 Cars on). "
        "<recomp-color primary>All Tracks</recomp-color> picks the computer cars from Rush 2's and Rush 2049's cars on "
        "every track. "
        "<recomp-color primary>Rush 2049 Tracks</recomp-color> races only Rush 2049 cars on the Rush 2049 tracks and "
        "only Rush 2 cars on the others. "
        "<recomp-color primary>Off</recomp-color> keeps the computer cars to Rush 2's cars; players can still pick "
        "Rush 2049 cars. Requires a Rush 2049 (USA) ROM.",
        {
            { rush2::car2049::DroneCars::AllTracks, "AllTracks", "All Tracks" },
            { rush2::car2049::DroneCars::Rush2049Tracks, "Rush2049Tracks", "Rush 2049 Tracks" },
            { rush2::car2049::DroneCars::Off, "Off", "Off" },
        },
        rush2::car2049::DroneCars::AllTracks
    );
    wings_config.add_option_change_callback(drones_option_id,
        [](recomp::config::ConfigValueVariant cur_value, recomp::config::ConfigValueVariant, recomp::config::OptionChangeContext) {
            rush2::car2049::set_drone_cars(static_cast<rush2::car2049::DroneCars>(std::get<uint32_t>(cur_value)));
        });

    wings_config.add_enum_option(
        speeds_option_id,
        "Car Speeds",
        "Rush 2049's cars accelerate harder than Rush 2's but top out lower, its computer cars drive faster for "
        "their cars, and its speedometer reads 20% over true speed. "
        "<recomp-color primary>Rush 2</recomp-color> tunes every car and the computer cars on every track to Rush "
        "2's. "
        "<recomp-color primary>Rush 2049</recomp-color> tunes them all to Rush 2049's, and the speedometer reads "
        "like Rush 2049's. "
        "Takes effect at the next race.",
        {
            { rush2::car2049::SpeedMode::Rush2, "Rush2", "Rush 2" },
            { rush2::car2049::SpeedMode::Rush2049, "Rush2049", "Rush 2049" },
        },
        rush2::car2049::SpeedMode::Rush2
    );
    wings_config.add_option_change_callback(speeds_option_id,
        [](recomp::config::ConfigValueVariant cur_value, recomp::config::ConfigValueVariant, recomp::config::OptionChangeContext) {
            rush2::car2049::set_speed_mode(static_cast<rush2::car2049::SpeedMode>(std::get<uint32_t>(cur_value)));
        });

    wings_config.add_bool_option(
        odometer_option_id,
        "Odometer",
        "Shows Rush 2049's odometer with each player's map (under the race time without one): the miles their car "
        "has driven this race, to a tenth, on rolling digits (kilometers with the speedometer in km/h). With 3 or 4 "
        "players the race time and place shrink to fit it. Requires a Rush 2049 (USA) ROM.",
        false
    );
    wings_config.add_option_change_callback(odometer_option_id,
        [](recomp::config::ConfigValueVariant cur_value, recomp::config::ConfigValueVariant, recomp::config::OptionChangeContext) {
            rush2::odometer2049::set_option(std::get<bool>(cur_value));
        });

    wings_config.add_enum_option(
        battle_time_option_id,
        "Battle Time Limit",
        "How long a Rush 2049 battle arena lasts (Start Game > Battle). Takes effect at the next race. Requires a Rush "
        "2049 (USA) ROM.",
        {
            { rush2::battle::TimeLimit::One, "One", "1 Minute" },
            { rush2::battle::TimeLimit::Two, "Two", "2 Minutes" },
            { rush2::battle::TimeLimit::Three, "Three", "3 Minutes" },
            { rush2::battle::TimeLimit::Five, "Five", "5 Minutes" },
            { rush2::battle::TimeLimit::Ten, "Ten", "10 Minutes" },
        },
        rush2::battle::TimeLimit::Three
    );
    wings_config.add_option_change_callback(battle_time_option_id,
        [](recomp::config::ConfigValueVariant cur_value, recomp::config::ConfigValueVariant, recomp::config::OptionChangeContext) {
            rush2::battle::set_time_limit(static_cast<rush2::battle::TimeLimit>(std::get<uint32_t>(cur_value)));
        });
    // Rush 2049 has each player pick a team color for a battle (0x8012E67C).
    for (int player = 0; player < 4; player++) {
        std::string id = battle_team_option_prefix + std::to_string(player + 1);
        wings_config.add_enum_option(
            id,
            "Player " + std::to_string(player + 1) + " Battle Team",
            "This player's team in a Rush 2049 battle arena. Cars of the same team don't damage each other and share a "
            "color. By default every player is a team of their own.",
            {
                { 0u, "Blue", "Blue" },
                { 1u, "Red", "Red" },
                { 2u, "Yellow", "Yellow" },
                { 3u, "Green", "Green" },
            },
            (uint32_t)player
        );
        wings_config.add_option_change_callback(id,
            [player](recomp::config::ConfigValueVariant cur_value, recomp::config::ConfigValueVariant, recomp::config::OptionChangeContext) {
                rush2::battle::set_team(player, (int)std::get<uint32_t>(cur_value));
            });
    }
    // Rush 2049 has each player pick one of three wings on the car setup screen; here it is the car select's WINGS
    // row (src/rush2049/wings_menu.cpp), which keeps each player's choice in these options.
    for (int player = 0; player < rush2::wings::max_players; player++) {
        std::string id = style_option(player);
        wings_config.add_enum_option(
            id,
            "Player " + std::to_string(player + 1) + " Wings",
            "Chooses this player's wings, as on Rush 2049's car setup screen. "
            "<recomp-color primary>Style 1</recomp-color> steers in the air. "
            "<recomp-color primary>Style 2</recomp-color> steers harder and glides, but slows the car. "
            "<recomp-color primary>Style 3</recomp-color> steers hardest and glides farthest.",
            {
                { 0u, "Style1", "Style 1" },
                { 1u, "Style2", "Style 2" },
                { 2u, "Style3", "Style 3" },
            },
            0u,
            true    // Not listed in a tab: set on the car select.
        );
        wings_config.add_option_change_callback(id,
            [player](recomp::config::ConfigValueVariant cur_value, recomp::config::ConfigValueVariant, recomp::config::OptionChangeContext) {
                rush2::wings::set_player_style(player, (int)std::get<uint32_t>(cur_value));
            });
    }
}

void rush2::wings::add_games_section(rush2::ui::OptionsPage* page, std::function<void()>& refresh) {
    recompui::ContextId context = recompui::get_current_context();
    rush2::ui::OptionsPage::Heading heading = page->add_heading("Rush 2049", rom_status_text());
    auto* button = context.create_element<recompui::Button>(heading.row, "Select ROM", recompui::ButtonStyle::Secondary);
    button->add_pressed_callback(select_rom);
    for (const std::string& id : { source_option_id, wings_option_id, tracks_option_id, cars_option_id, drones_option_id,
                                    speeds_option_id, odometer_option_id }) {
        page->add_option(wings_config, id);
    }
    refresh = [note = heading.note, shown = rom_status_text()]() mutable {
        std::string text = rom_status_text();
        if (text != shown) {
            shown = std::move(text);
            note->set_text(shown);
        }
    };
}

void rush2::wings::save_config() {
    wings_config.save_config();
}

int rush2::wings::get_style_option(int player) {
    return (int)std::get<uint32_t>(wings_config.get_option_value(style_option(player)));
}

void rush2::wings::set_style_option(int player, int style) {
    wings_config.update_option_value(style_option(player), (uint32_t)style);
    wings_config.save_config();
}

// Runs after recompui::config::finalize() has registered the config path.
void rush2::wings::load_config() {
    wings_config.load_config();
    load_chosen_source();
    config_loaded = true;
    update_rom_ui();
}

bool rush2::wings::rom_to_big_endian(std::vector<uint8_t>& data) {
    return to_big_endian(data);
}
