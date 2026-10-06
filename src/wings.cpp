// Rush 2049 wings: settings (shown in the Games and Players tabs) and Rush 2049 ROM handling.
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
#include "../lib/N64ModernRuntime/thirdparty/json/json.hpp"

#include <array>
#include <atomic>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <vector>

#include "recompui/recompui.h"
#include "recompui/config.h"
#include "librecomp/config.hpp"
#include "util/file.h"
#include "elements/ui_button.h"
#include "elements/ui_label.h"
#include "options_page.h"

#include "rush2.h"
#include "car2049.h"
#include "track2049.h"
#include "wings_internal.h"

namespace {
    const std::string config_id = "rush2049";
    const std::string wings_option_id = "wings";
    const std::string tracks_option_id = "tracks";
    const std::string cars_option_id = "cars";
    const std::string drones_option_id = "computer_cars";
    const std::string speeds_option_id = "car_speeds";
    const std::string style_option_p1 = "wing_style_p1";
    const std::string style_option_p2 = "wing_style_p2";
    const char* rom_file_name = "rush2049.z64";

    constexpr size_t rom_size = 0xC00000;
    // SHA-1 of San Francisco Rush 2049 (USA) in big-endian byte order.
    constexpr std::array<uint8_t, 20> rom_sha1 = {
        0x3f, 0x99, 0x35, 0x1d, 0x7b, 0xb6, 0x16, 0x56, 0x61, 0x4b,
        0xdb, 0x2a, 0xa1, 0xa9, 0x0c, 0xfe, 0x55, 0xd1, 0x92, 0x2c,
    };

    // Owned here instead of through create_config_tab: its options are shown in the Games tab (src/games_tab.cpp)
    // under the ROM picker, and the wing styles in the Players tab.
    recomp::config::Config wings_config{ "Rush 2049", config_id, false };

    std::mutex rom_mutex;
    std::shared_ptr<const std::vector<uint8_t>> rom_data; // Big-endian ROM, null until a valid one is loaded.
    std::atomic_bool wings_option = false;

    std::array<uint8_t, 20> sha1(const std::vector<uint8_t>& data) {
        uint32_t h[5] = { 0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0 };
        auto rol = [](uint32_t v, int n) { return (v << n) | (v >> (32 - n)); };

        std::vector<uint8_t> msg = data;
        uint64_t bit_len = uint64_t(data.size()) * 8;
        msg.push_back(0x80);
        while (msg.size() % 64 != 56) {
            msg.push_back(0);
        }
        for (int i = 7; i >= 0; i--) {
            msg.push_back(uint8_t(bit_len >> (i * 8)));
        }

        for (size_t chunk = 0; chunk < msg.size(); chunk += 64) {
            uint32_t w[80];
            for (int i = 0; i < 16; i++) {
                const uint8_t* p = &msg[chunk + i * 4];
                w[i] = (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
            }
            for (int i = 16; i < 80; i++) {
                w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
            }
            uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
            for (int i = 0; i < 80; i++) {
                uint32_t f, k;
                if (i < 20) { f = (b & c) | (~b & d); k = 0x5A827999; }
                else if (i < 40) { f = b ^ c ^ d; k = 0x6ED9EBA1; }
                else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDC; }
                else { f = b ^ c ^ d; k = 0xCA62C1D6; }
                uint32_t temp = rol(a, 5) + f + e + k + w[i];
                e = d; d = c; c = rol(b, 30); b = a; a = temp;
            }
            h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e;
        }

        std::array<uint8_t, 20> out;
        for (int i = 0; i < 5; i++) {
            out[i * 4 + 0] = uint8_t(h[i] >> 24);
            out[i * 4 + 1] = uint8_t(h[i] >> 16);
            out[i * 4 + 2] = uint8_t(h[i] >> 8);
            out[i * 4 + 3] = uint8_t(h[i]);
        }
        return out;
    }

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
        if (out.size() != rom_size || sha1(out) != rom_sha1) {
            return RomCheck::WrongVersion;
        }
        return RomCheck::Good;
    }

    std::filesystem::path stored_rom_path() {
        return recompui::file::get_app_folder_path() / rom_file_name;
    }

    void set_rom(std::shared_ptr<const std::vector<uint8_t>> rom) {
        {
            std::lock_guard lock{ rom_mutex };
            rom_data = rom;
        }
        rush2::wings::on_rom_changed();
    }

    std::string rom_status_text() {
        return rush2::wings::rom_available()
            ? "ROM found."
            : "Needs a San Francisco Rush 2049 (USA) ROM for its tracks, cars and wings.";
    }

    void update_rom_ui() {
        bool disabled = !rush2::wings::rom_available();
        wings_config.update_option_disabled(wings_option_id, disabled);
        wings_config.update_option_disabled(tracks_option_id, disabled);
        wings_config.update_option_disabled(cars_option_id, disabled);
        wings_config.update_option_disabled(drones_option_id, disabled);
        wings_config.update_option_disabled(style_option_p1, disabled);
        wings_config.update_option_disabled(style_option_p2, disabled);
    }

    void select_rom() {
        recompui::file::open_file_dialog([](bool success, const std::filesystem::path& path) {
            if (!success) {
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
                        "Wings require the NTSC-U (USA) N64 version.");
                    return;
            }

            std::error_code ec;
            std::filesystem::create_directories(recompui::file::get_app_folder_path(), ec);
            std::ofstream out{ stored_rom_path(), std::ios::binary };
            if (!out.write(reinterpret_cast<const char*>(data->data()), data->size())) {
                recompui::message_box("Failed to copy the Rush 2049 ROM into the app folder.");
                return;
            }
            out.close();

            set_rom(data);
            update_rom_ui();
        });
    }
}

std::shared_ptr<const std::vector<uint8_t>> rush2::wings::get_rom() {
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

    // Rush 2049 has each player pick one of three wings on the car setup screen.
    for (int player = 0; player < 2; player++) {
        std::string id = player == 0 ? style_option_p1 : style_option_p2;
        wings_config.add_enum_option(
            id,
            player == 0 ? "Player 1 Wings" : "Player 2 Wings",
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
            true    // Shown in the Players tab (src/players_tab.cpp).
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
    for (const std::string& id : { wings_option_id, tracks_option_id, cars_option_id, drones_option_id,
                                    speeds_option_id }) {
        page->add_option(wings_config, id);
    }
    refresh = [note = heading.note, shown = rush2::wings::rom_available()]() mutable {
        if (rush2::wings::rom_available() != shown) {
            shown = !shown;
            note->set_text(rom_status_text());
        }
    };
}

void rush2::wings::save_config() {
    wings_config.save_config();
}

int rush2::wings::get_style_option(int player) {
    return (int)std::get<uint32_t>(wings_config.get_option_value(player == 0 ? style_option_p1 : style_option_p2));
}

void rush2::wings::set_style_option(int player, int style) {
    wings_config.update_option_value(player == 0 ? style_option_p1 : style_option_p2, (uint32_t)style);
    wings_config.save_config();
}

// Runs after recompui::config::finalize() has registered the config path.
void rush2::wings::load_config() {
    wings_config.load_config();

    std::vector<uint8_t> data;
    std::filesystem::path path = stored_rom_path();
    if (std::filesystem::exists(path)) {
        if (read_rom(path, data) == RomCheck::Good) {
            set_rom(std::make_shared<const std::vector<uint8_t>>(std::move(data)));
        }
        else {
            printf("[Wings] Ignoring %s: not a valid Rush 2049 (USA) ROM\n", path.string().c_str());
        }
    }
    update_rom_ui();
}

// Shared with the SF Rush ROM picker (src/rush1_rom.cpp).
std::array<uint8_t, 20> rush2::wings::rom_sha1(const std::vector<uint8_t>& data) {
    return sha1(data);
}

bool rush2::wings::rom_to_big_endian(std::vector<uint8_t>& data) {
    return to_big_endian(data);
}
