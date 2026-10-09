// SF Rush settings (shown in the Games tab) and San Francisco Rush (Rush 1) ROM handling.
//
// The Rush 1 tracks are converted from the user's own San Francisco Rush (USA) ROM, so nothing from that game ships
// with the recomp. The ROM is chosen with a button on the Games tab, checked, and copied in big-endian (.z64) byte
// order to the app folder as rush1.z64; the SF Rush Tracks option stays disabled until a valid ROM is present.
//
// Rush 1's main code is LZ compressed (Rush 2's variant) at ROM 0x7A7930 and runs at 0x8005BB10. Its 72 assets are
// listed by ROM offset at 0x800C7C1C and are all LZ compressed (docs/rush1_research.md).

// As in src/rush2049/wings.cpp: librecomp's nlohmann::json first.
#include "../../lib/N64ModernRuntime/thirdparty/json/json.hpp"

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

#include "car1_stripes.h"
#include "track1.h"
#include "track2049_convert.h"
#include "wings_internal.h"

namespace {
    // In the Games tab's config, games.json (src/rush2049/wings.cpp).
    const std::string tracks_option_id = "sfrush_tracks";
    const std::string stripes_option_id = "sfrush_car_stripes";
    const char* rom_file_name = "rush1.z64";

    constexpr size_t rom_size = 0x800000;
    // SHA-1 of San Francisco Rush - Extreme Racing (USA) in big-endian byte order.
    constexpr std::array<uint8_t, 20> rom_sha1 = {
        0xcc, 0x62, 0x53, 0x9c, 0xb3, 0x0b, 0x18, 0x0c, 0x3c, 0x7e,
        0x0a, 0xa9, 0x27, 0x78, 0x6e, 0xd0, 0x61, 0xd8, 0xd9, 0xab,
    };
    constexpr uint32_t main_rom = 0x7A7930;
    constexpr uint32_t asset_table = 0x800C7C1C;
    constexpr int asset_count = 72;

    std::mutex rom_mutex;
    std::shared_ptr<const std::vector<uint8_t>> rom_data;
    std::atomic_bool tracks_option = true;

    std::mutex main_mutex;
    const std::vector<uint8_t>* main_rom_data = nullptr;
    std::shared_ptr<const std::vector<uint8_t>> main_data;

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
        if (!rush2::wings::rom_to_big_endian(out)) {
            return RomCheck::NotARom;
        }
        // Game code at 0x3B: "NSF" is San Francisco Rush in any region; the last byte is the region.
        if (memcmp(&out[0x3B], "NSF", 3) != 0) {
            return RomCheck::WrongGame;
        }
        if (out.size() != rom_size || rush2::wings::rom_sha1(out) != rom_sha1) {
            return RomCheck::WrongVersion;
        }
        return RomCheck::Good;
    }

    std::filesystem::path stored_rom_path() {
        return recompui::file::get_app_folder_path() / rom_file_name;
    }

    void set_rom(std::shared_ptr<const std::vector<uint8_t>> rom) {
        std::lock_guard lock{ rom_mutex };
        rom_data = rom;
    }

    std::string rom_status_text() {
        return rush2::track1::rom_available()
            ? "ROM found."
            : "Needs a San Francisco Rush (USA) ROM for its tracks.";
    }

    void update_rom_ui() {
        rush2::wings::games_config().update_option_disabled(tracks_option_id, !rush2::track1::rom_available());
        rush2::wings::games_config().update_option_disabled(stripes_option_id, !rush2::track1::rom_available());
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
                    recompui::message_box("This ROM is not San Francisco Rush.");
                    return;
                case RomCheck::WrongVersion:
                    recompui::message_box("This ROM is San Francisco Rush, but the wrong version.\n"
                        "The tracks require the NTSC-U (USA) N64 version.");
                    return;
            }

            std::error_code ec;
            std::filesystem::create_directories(recompui::file::get_app_folder_path(), ec);
            std::ofstream out{ stored_rom_path(), std::ios::binary };
            if (!out.write(reinterpret_cast<const char*>(data->data()), data->size())) {
                recompui::message_box("Failed to copy the SF Rush ROM into the app folder.");
                return;
            }
            out.close();

            set_rom(data);
            update_rom_ui();
        });
    }

    uint32_t be32(const std::vector<uint8_t>& d, size_t o) {
        return (uint32_t(d[o]) << 24) | (uint32_t(d[o + 1]) << 16) | (uint32_t(d[o + 2]) << 8) | d[o + 3];
    }
}

std::shared_ptr<const std::vector<uint8_t>> rush2::track1::get_rom() {
    std::lock_guard lock{ rom_mutex };
    return rom_data;
}

bool rush2::track1::rom_available() {
    std::lock_guard lock{ rom_mutex };
    return rom_data != nullptr;
}

void rush2::track1::set_option(bool enabled) {
    tracks_option = enabled;
}

bool rush2::track1::available() {
    return tracks_option.load(std::memory_order_relaxed) && rom_available();
}

std::shared_ptr<const std::vector<uint8_t>> rush2::track1::main_code(const std::vector<uint8_t>& rom) {
    std::lock_guard lock{ main_mutex };
    if (main_rom_data != &rom || main_data == nullptr) {
        auto data = std::make_shared<std::vector<uint8_t>>();
        main_rom_data = &rom;
        if (rom.size() <= main_rom || !rush2::track2049::rush2_lz_decompress(rom.data() + main_rom, rom.size() - main_rom, *data) ||
            data->size() < asset_table - main_vram + asset_count * 4) {
            printf("[Rush1] Couldn't read the main code\n");
            main_data = nullptr;
            return nullptr;
        }
        main_data = data;
    }
    return main_data;
}

bool rush2::track1::read_asset(const std::vector<uint8_t>& rom, int index, std::vector<uint8_t>& out) {
    if (index < 0 || index >= asset_count) {
        return false;
    }
    auto main = main_code(rom);
    if (main == nullptr) {
        return false;
    }
    uint32_t offset = be32(*main, asset_table - main_vram + index * 4);
    if (offset >= rom.size() || !rush2::track2049::rush2_lz_decompress(rom.data() + offset, rom.size() - offset, out)) {
        printf("[Rush1] Failed to decompress asset %d\n", index);
        return false;
    }
    return true;
}

void rush2::track1::init_config() {
    recomp::config::Config& rush1_config = rush2::wings::games_config();
    rush1_config.add_bool_option(
        tracks_option_id,
        "SF Rush Tracks",
        "Adds the seven race tracks of San Francisco Rush to the track select, after Rush 2's and Rush 2049's tracks. "
        "Requires a San Francisco Rush (USA) ROM.",
        true
    );
    rush1_config.add_option_change_callback(tracks_option_id,
        [](recomp::config::ConfigValueVariant cur_value, recomp::config::ConfigValueVariant, recomp::config::OptionChangeContext) {
            rush2::track1::set_option(std::get<bool>(cur_value));
        });
    rush1_config.add_bool_option(
        stripes_option_id,
        "SF Rush Car Stripes",
        "Adds a ninth STRIPE choice, SF RUSH, to the cars whose paint in San Francisco Rush had a pattern Rush 2 lacks: "
        "the Camaro's and Hot Rod's flames, the Taxi's checker band and the VW Bus's swirls. It is drawn in the "
        "STRIPE COLOR. Requires a San Francisco Rush (USA) ROM.",
        true
    );
    rush1_config.add_option_change_callback(stripes_option_id,
        [](recomp::config::ConfigValueVariant cur_value, recomp::config::ConfigValueVariant, recomp::config::OptionChangeContext) {
            rush2::car1stripes::set_option(std::get<bool>(cur_value));
        });
}

void rush2::track1::add_games_section(rush2::ui::OptionsPage* page, std::function<void()>& refresh) {
    recompui::ContextId context = recompui::get_current_context();
    rush2::ui::OptionsPage::Heading heading = page->add_heading("San Francisco Rush", rom_status_text());
    auto* button = context.create_element<recompui::Button>(heading.row, "Select ROM", recompui::ButtonStyle::Secondary);
    button->add_pressed_callback(select_rom);
    page->add_option(rush2::wings::games_config(), tracks_option_id);
    page->add_option(rush2::wings::games_config(), stripes_option_id);
    refresh = [note = heading.note, shown = rush2::track1::rom_available()]() mutable {
        if (rush2::track1::rom_available() != shown) {
            shown = !shown;
            note->set_text(rom_status_text());
        }
    };
}

// Runs after rush2::wings::load_config(), which loads the option.
void rush2::track1::load_config() {
    std::vector<uint8_t> data;
    std::filesystem::path path = stored_rom_path();
    if (std::filesystem::exists(path)) {
        if (read_rom(path, data) == RomCheck::Good) {
            set_rom(std::make_shared<const std::vector<uint8_t>>(std::move(data)));
        }
        else {
            printf("[Rush1] Ignoring %s: not a valid San Francisco Rush (USA) ROM\n", path.string().c_str());
        }
    }
    update_rom_ui();
}
