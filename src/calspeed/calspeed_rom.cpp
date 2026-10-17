// California Speed settings (shown in the Games tab) and California Speed ROM handling.
//
// The Cal Speed tracks are converted from the user's own California Speed (USA) ROM, so nothing from that game ships
// with the recomp. The ROM is chosen with a button on the Games tab, checked, and copied in big-endian (.z64) byte
// order to the app folder as calspeed.z64; the California Speed Tracks option stays disabled until a valid ROM is
// present.
//
// California Speed is built on the Rush 2 engine (docs/calspeed_research.md). Its main code is raw in the ROM (not
// compressed, unlike Rush 2's and SF Rush's) at 0x1CD60 and runs at 0x80053710. Its 128 assets are listed by ROM offset
// at 0x8008DFBC (ROM 0x5760C) and are all compressed with Rush 2's LZ variant.

// As in src/rush2049/wings.cpp: librecomp's nlohmann::json first.
#include "../../lib/N64ModernRuntime/thirdparty/json/json.hpp"

#include <algorithm>
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
#include "librecomp/game.hpp"
#include "util/file.h"
#include "elements/ui_button.h"
#include "elements/ui_label.h"
#include "options_page.h"

#include "recomp.h"
#include "calspeed.h"
#include "track2049_convert.h"
#include "track_cache.h"
#include "wings_internal.h"

namespace {
    // In the Games tab's config, games.json (src/rush2049/wings.cpp).
    const std::string tracks_option_id = "calspeed_tracks";
    const std::string cars_option_id = "calspeed_cars";
    const std::string drones_option_id = "calspeed_computer_cars";
    const char* rom_file_name = "calspeed.z64";

    constexpr size_t rom_size = 0x1000000;
    // SHA-1 of California Speed (USA) in big-endian byte order.
    constexpr std::array<uint8_t, 20> rom_sha1 = {
        0xbd, 0x4c, 0x07, 0x0a, 0x71, 0xef, 0x58, 0x49, 0x95, 0x87,
        0xcb, 0x81, 0x1f, 0xb7, 0x49, 0x0b, 0x88, 0xdd, 0x7c, 0x0b,
    };
    constexpr uint32_t main_rom = 0x1CD60;
    constexpr uint32_t main_vram = 0x80053710;
    constexpr uint32_t main_end_rom = 0xB1C70;      // The first asset: the raw segment's code and data end here.
    constexpr uint32_t asset_table = 0x8008DFBC;
    constexpr int asset_count = 128;

    std::mutex rom_mutex;
    std::shared_ptr<const std::vector<uint8_t>> rom_data;
    std::atomic_bool tracks_option = true;

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
        // Game code at 0x3B: "NCL" is California Speed in any region; the last byte is the region.
        if (memcmp(&out[0x3B], "NCL", 3) != 0) {
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

    void set_rom(std::shared_ptr<const std::vector<uint8_t>> rom) {
        std::lock_guard lock{ rom_mutex };
        rom_data = rom;
    }

    std::string rom_status_text() {
        return rush2::calspeed::rom_available()
            ? "ROM found."
            : "Needs a California Speed (USA) ROM for its tracks and cars.";
    }

    void update_rom_ui() {
        recomp::config::Config& config = rush2::wings::games_config();
        bool disabled = !rush2::calspeed::rom_available();
        config.update_option_disabled(tracks_option_id, disabled);
        config.update_option_disabled(cars_option_id, disabled);
        // The computer cars' choice is among California Speed's cars.
        bool cars_on = std::get<bool>(config.get_option_value(cars_option_id));
        config.update_option_disabled(drones_option_id, disabled || !cars_on);
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
                    recompui::message_box("This ROM is not California Speed.");
                    return;
                case RomCheck::WrongVersion:
                    recompui::message_box("This ROM is California Speed, but the wrong version.\n"
                        "The tracks require the NTSC-U (USA) N64 version.");
                    return;
            }

            std::error_code ec;
            std::filesystem::create_directories(recomp::get_config_path(), ec);
            std::ofstream out{ stored_rom_path(), std::ios::binary };
            if (!out.write(reinterpret_cast<const char*>(data->data()), data->size())) {
                recompui::message_box("Failed to copy the California Speed ROM into the app folder.");
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

    std::atomic_int raced_track = 0;
}

std::shared_ptr<const std::vector<uint8_t>> rush2::calspeed::get_rom() {
    std::lock_guard lock{ rom_mutex };
    return rom_data;
}

bool rush2::calspeed::rom_available() {
    std::lock_guard lock{ rom_mutex };
    return rom_data != nullptr;
}

void rush2::calspeed::set_option(bool enabled) {
    tracks_option = enabled;
}

bool rush2::calspeed::available() {
    return tracks_option.load(std::memory_order_relaxed) && rom_available();
}

uint32_t rush2::calspeed::main_word(const std::vector<uint8_t>& rom, uint32_t vram) {
    if (vram < main_vram || (vram & 3) != 0) {
        return 0;
    }
    size_t o = main_rom + size_t(vram - main_vram);
    return o + 4 <= main_end_rom && o + 4 <= rom.size() ? be32(rom, o) : 0;
}

uint64_t rush2::calspeed::rom_source(const std::vector<uint8_t>& rom) {
    static std::mutex hash_mutex;
    static const std::vector<uint8_t>* hashed_rom = nullptr;
    static uint64_t rom_hash = 0;
    std::lock_guard lock{ hash_mutex };
    if (hashed_rom != &rom) {
        rom_hash = rush2::track_cache::hash(rom.data(), rom.size(), 0xC5);
        hashed_rom = &rom;
    }
    return rom_hash;
}

bool rush2::calspeed::read_asset(const std::vector<uint8_t>& rom, int index, std::vector<uint8_t>& out) {
    if (index < 0 || index >= asset_count) {
        return false;
    }
    uint32_t offset = main_word(rom, asset_table + uint32_t(index) * 4);
    if (offset == 0 || offset >= rom.size() ||
        !rush2::track2049::rush2_lz_decompress(rom.data() + offset, rom.size() - offset, out)) {
        printf("[CalSpeed] Failed to decompress asset %d\n", index);
        return false;
    }
    return true;
}

int rush2::calspeed::race_track() {
    return raced_track;
}

void rush2::calspeed::set_race_track(int k) {
    raced_track = k;
}

// Cars that fall under the course. Rush 2's tracks lie over an out-of-bounds floor (type 7: a wheel touching it
// wrecks the car, which puts it back on the road); California Speed's have none, and the ground runs on below its
// roads (Monterey's down to y -252). A drone glancing off the side of Monterey's ramp at (0, -10, 5630) flew 290 ft up
// and over the left wall and drove on 200 ft under the course for the rest of the race. On a California Speed track a
// car more than 60 ft below the nearest point of the racing line (the path's spine and AI lanes) for 2 s of race time
// is wrecked, as type 7 would (docs/calspeed_research.md, Conversion for the host slot). A road under another one is
// nearer its own points than the upper road's, so a car on it isn't counted.
namespace {
    constexpr int max_race_cars = 8;
    constexpr float under_depth = 60.0f, under_seconds = 2.0f;
    std::vector<std::array<float, 3>> race_points;   // the raced track's spine and AI lane points
    float under_since[max_race_cars];
    bool under[max_race_cars] = {};
}

void rush2::calspeed::set_race_path(const std::vector<uint8_t>& path) {
    race_points.clear();
    std::fill(std::begin(under), std::end(under), false);
    // Rush 2's path file (tools/rush2049/paths.py): route header 0x32C {u16 spine, u16, u32, u8 branches, ...}, the
    // branch records (16 bytes), u16 point total, the spine and branch points {s16 x, y, z}, then 4 lanes of
    // {u16 count, u8, u8, u32} + count x {s16 x, y, z, u8 mph, u8 code}.
    auto u16 = [&](size_t o) { return o + 2 <= path.size() ? uint16_t((path[o] << 8) | path[o + 1]) : uint16_t(0); };
    auto point = [&](size_t o) {
        return std::array<float, 3>{ float(int16_t(u16(o))), float(int16_t(u16(o + 2))), float(int16_t(u16(o + 4))) };
    };
    constexpr size_t route = 0x32C;
    if (path.size() < route + 16) return;
    size_t spine = u16(route), branches = path[route + 8];
    size_t o = route + 16 + branches * 16;
    size_t total = u16(o);
    o += 2;
    if (o + total * 6 > path.size()) return;
    for (size_t i = 0; i < spine; i++) race_points.push_back(point(o + i * 6));
    o += total * 6;
    for (int lane = 0; lane < 4 && o + 8 <= path.size(); lane++) {
        size_t count = u16(o);
        o += 8;
        for (size_t i = 0; i < count && o + i * 8 + 6 <= path.size(); i++) race_points.push_back(point(o + i * 8));
        o += count * 8;
    }
}

extern "C" void car_wreck_trigger_8006E1A8(uint8_t* rdram, recomp_context* ctx);

// Start of func_8005CFB0, each race car's per-frame callback ($a0 = its callback entry, car index at +0xE).
extern "C" void rush2_calspeed_car_frame(uint8_t* rdram, recomp_context* ctx) {
    constexpr uint32_t game_mode = 0x8010C0D0;               // 3 racing
    constexpr uint32_t race_time = 0x80117488;               // f32 seconds
    constexpr uint32_t car_slots = 0x801124A0, car_slot_size = 0x354; // car state: position (3 f32) at +0
    constexpr uint32_t slot_wrecked = 0x342, slot_out = 0x343;          // s8
    constexpr uint32_t physics_cars = 0x800F5470, physics_car_size = 0x81C;
    constexpr uint32_t car_placing = 0x6C8;                  // s16: >= 0 while the car is being placed
    constexpr uint32_t car_out = 0x6D1;                      // u8: set by the type 7 floor before the wreck
    if (raced_track <= 0 || race_points.empty() || MEM_W(0, (int32_t)game_mode) != 3) {
        return;
    }
    int car = (int16_t)MEM_H(0, (int32_t)((uint32_t)ctx->r4 + 0xE));
    if (car < 0 || car >= max_race_cars) {
        return;
    }
    int32_t slot = (int32_t)(car_slots + uint32_t(car) * car_slot_size);
    int32_t physics = (int32_t)(physics_cars + uint32_t(car) * physics_car_size);
    auto f32 = [&](int32_t at) {
        uint32_t bits = (uint32_t)MEM_W(0, at);
        float f;
        memcpy(&f, &bits, 4);
        return f;
    };
    float x = f32(slot), y = f32(slot + 4), z = f32(slot + 8);
    // The nearest racing-line point, height counted at half weight (as tools/race_watch.py places a car).
    float best = 1e30f, best_y = y;
    for (const auto& p : race_points) {
        float dx = p[0] - x, dy = (p[1] - y) * 0.5f, dz = p[2] - z;
        float d = dx * dx + dy * dy + dz * dz;
        if (d < best) {
            best = d;
            best_y = p[1];
        }
    }
    float now = f32((int32_t)race_time);
    if (y > best_y - under_depth || MEM_B(0, (int32_t)(slot + slot_wrecked)) != 0 ||
        MEM_B(0, (int32_t)(slot + slot_out)) != 0 || (int16_t)MEM_H(0, (int32_t)(physics + car_placing)) >= 0) {
        under[car] = false;
        return;
    }
    if (!under[car]) {
        under[car] = true;
        under_since[car] = now;
        return;
    }
    if (now - under_since[car] < under_seconds) {
        return;
    }
    under[car] = false;
    recomp_context saved = *ctx;
    MEM_B(0, (int32_t)(physics + car_out)) = 1;
    ctx->r29 = (uint64_t)(int64_t)(int32_t)((uint32_t)ctx->r29 - 0x20); // func_8005CFB0's frame isn't set up yet
    ctx->r4 = (uint64_t)car;
    ctx->r5 = 1;
    car_wreck_trigger_8006E1A8(rdram, ctx);
    *ctx = saved;
}

void rush2::calspeed::init_config() {
    recomp::config::Config& config = rush2::wings::games_config();
    config.add_bool_option(
        tracks_option_id,
        "California Speed Tracks",
        "Adds the sixteen tracks of California Speed to the track select, after the custom tracks. Its point-to-point "
        "courses are raced over one lap. Requires a California Speed (USA) ROM.",
        true
    );
    config.add_option_change_callback(tracks_option_id,
        [](recomp::config::ConfigValueVariant cur_value, recomp::config::ConfigValueVariant, recomp::config::OptionChangeContext) {
            rush2::calspeed::set_option(std::get<bool>(cur_value));
        });
    config.add_bool_option(
        cars_option_id,
        "California Speed Cars",
        "Adds the twenty-one cars of California Speed to the car select, after the other cars, for players and the "
        "computer cars. Takes effect the next time the car select opens. Requires a California Speed (USA) ROM.",
        true
    );
    config.add_option_change_callback(cars_option_id,
        [](recomp::config::ConfigValueVariant cur_value, recomp::config::ConfigValueVariant, recomp::config::OptionChangeContext) {
            rush2::calspeed::set_cars_option(std::get<bool>(cur_value));
            update_rom_ui();
        });
    config.add_enum_option(
        drones_option_id,
        "California Speed Computer Cars",
        "Sets where the computer cars drive California Speed cars (with California Speed Cars on). "
        "<recomp-color primary>All Tracks</recomp-color> adds California Speed's cars to the computer cars' picks on "
        "every track. "
        "<recomp-color primary>California Speed Tracks</recomp-color> races only California Speed cars on the "
        "California Speed tracks. "
        "<recomp-color primary>Off</recomp-color> keeps them out of the computer cars; players can still pick them. "
        "Requires a California Speed (USA) ROM.",
        {
            { rush2::car2049::DroneCars::AllTracks, "AllTracks", "All Tracks" },
            { rush2::car2049::DroneCars::Rush2049Tracks, "CalSpeedTracks", "California Speed Tracks" },
            { rush2::car2049::DroneCars::Off, "Off", "Off" },
        },
        rush2::car2049::DroneCars::AllTracks
    );
    config.add_option_change_callback(drones_option_id,
        [](recomp::config::ConfigValueVariant cur_value, recomp::config::ConfigValueVariant, recomp::config::OptionChangeContext) {
            rush2::calspeed::set_drone_cars(static_cast<rush2::car2049::DroneCars>(std::get<uint32_t>(cur_value)));
        });
}

void rush2::calspeed::add_games_section(rush2::ui::OptionsPage* page, std::function<void()>& refresh) {
    recompui::ContextId context = recompui::get_current_context();
    rush2::ui::OptionsPage::Heading heading = page->add_heading("California Speed", rom_status_text());
    auto* button = context.create_element<recompui::Button>(heading.row, "Select ROM", recompui::ButtonStyle::Secondary);
    button->add_pressed_callback(select_rom);
    page->add_option(rush2::wings::games_config(), tracks_option_id);
    page->add_option(rush2::wings::games_config(), cars_option_id);
    page->add_option(rush2::wings::games_config(), drones_option_id);
    refresh = [note = heading.note, shown = rush2::calspeed::rom_available()]() mutable {
        if (rush2::calspeed::rom_available() != shown) {
            shown = !shown;
            note->set_text(rom_status_text());
        }
    };
}

// Runs after rush2::wings::load_config(), which loads the option.
void rush2::calspeed::load_config() {
    std::vector<uint8_t> data;
    std::filesystem::path path = stored_rom_path();
    if (std::filesystem::exists(path)) {
        if (read_rom(path, data) == RomCheck::Good) {
            set_rom(std::make_shared<const std::vector<uint8_t>>(std::move(data)));
        }
        else {
            printf("[CalSpeed] Ignoring %s: not a valid California Speed (USA) ROM\n", path.string().c_str());
        }
    }
    update_rom_ui();
}
