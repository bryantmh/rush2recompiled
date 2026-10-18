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
#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
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
#include "track2049.h"
#include "track2049_convert.h"
#include "track_cache.h"
#include "wings_internal.h"

namespace {
    // In the Games tab's config, games.json (src/rush2049/wings.cpp).
    const std::string tracks_option_id = "calspeed_tracks";
    const std::string cars_option_id = "calspeed_cars";
    const std::string drones_option_id = "calspeed_computer_cars";
    const std::string road_hold_option_id = "calspeed_road_hold";
    // The California Speed Road Hold option: off, its tracks have Rush 2's own gravity only.
    std::atomic_bool road_hold_on = true;
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
        config.update_option_disabled(road_hold_option_id, disabled);
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
// car more than 60 ft below the nearest point of the racing line (the path's spine, branches and AI lanes) for 2 s of race time
// is wrecked, as type 7 would (docs/calspeed_research.md, Conversion for the host slot). A road under another one is
// nearer its own points than the upper road's, so a car on it isn't counted.
namespace {
    constexpr int max_race_cars = 8;
    constexpr float under_depth = 60.0f, under_seconds = 2.0f;
    std::vector<std::array<float, 3>> race_points;   // the raced track's spine, branch and AI lane points
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
    size_t branches = path[route + 8];
    size_t o = route + 16 + branches * 16;
    size_t total = u16(o);
    o += 2;
    if (o + total * 6 > path.size()) return;
    // The spine and every branch: alternate routes run well below the main road (Monterey's up to 127 ft).
    for (size_t i = 0; i < total; i++) race_points.push_back(point(o + i * 6));
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

// Keeping cars down on California Speed's tracks (docs/calspeed_research.md#road-hold). Its tracks' hills and jumps
// are built for its road hold, under which a car in the air falls at about 4.6 g; at Rush 2's 1 g cars fly off them.
// In car_physics_forces after car_force_sum ($s0 = the car): the forces in the car's frame are summed at +0x4.
namespace {
    constexpr uint32_t car_force = 0x4;           // f32[3], the car's frame
    constexpr uint32_t car_gravity = 0x200;       // f32[3]: gravity (+0x2C0, world) in the car's frame
    constexpr uint32_t car_weight = 0x5AC;        // f32
    constexpr uint32_t car_compression = 0x5BC;   // f32[4], clamped to +-3 by car_ground_contact; below 0 = off the road
    constexpr uint32_t car_matrix = 0x2CC;        // f32[3][3]: world -> the car's frame
    constexpr uint32_t car_wheel_polygon = 0x2F0; // f32[3][3] per wheel (0x24 apart): world -> its road polygon's frame

    // Which one runs (docs/calspeed_research.md#road-hold):
    // - Copy: California Speed's road hold as it is (road_hold), with the springs holding its push up: each wheel's
    //   support of the body (car_wheel_force_loop's load at 0x34($sp), rush2_calspeed_wheel_support) x push / weight
    //   lifts the car at its center, so it rides at Rush 2's height with Rush 2's roll, pitch and tire loads, and grips
    //   as on any other track. Only where the road falls away does the push show: the support lets go while the
    //   push keeps pulling, and the car follows the road as California Speed's do. The damper's jump at polygon edges is cut as well (rush2_calspeed_wheel_damper).
    // - Shape: a push from the road's curvature (shape_push): on a crest the push that keeps the car on it, nothing on
    //   flat road, grades, dips and corners. The car still lifted a little over polygon edges.
    // - Follow (removed): a push from the tires' unloading; the loads jump every step (damping), so it snapped
    //   between 0 and 2 x weight on ordinary bumps.
    // - AirPull: only what a wheel adds once it leaves the road, over the 0.32 it gives on the road. It starts after
    //   the car has begun to fly, so the car lifted and was then yanked down.
    // - Full: the whole road hold. Its ground push (1.28 x weight) sinks Rush 2's springs (a third as stiff as
    //   California Speed's) from about 0.42 to 0.7 ft and loads the tires: every car corners differently.
    // - AirGravity: 3.6 x weight more once the lowest wheel is 0.25-1.25 ft up: at takeoff it went from nothing to
    //   4.6 g, which felt like being slammed down.
    enum class Hold { Gravity, Copy, Shape, AirPull, Full, AirGravity };
    constexpr Hold hold = Hold::Gravity;

    // Gravity: California Speed's world as a heavier one, with the car built to match, so on the road it drives as on
    // any other track (gravity_push, rush2_calspeed_wheel_damper, rush2_calspeed_tire_load). California Speed is
    // built this way: its road hold adds 4 x 0.32 = 1.28 g on the road (2.28 g in all) and up to 3.6 more with the
    // wheels well up (4.6 g), and its cars' springs per unit of mass are 2.27 times Rush 2's (-10620 against -3600 at
    // 1.3 times the mass) and their dampers 1.56 times (sqrt 2.28 = 1.51), so they sit at Rush 2's height under it.
    // - gravity x the road hold's factors (road_hold_push: 1 + 1.28 on the road, up to 4.6 in the air);
    // - springs and anti-roll x `gravity_scale` (the road's 2.28): the car sits at Rush 2's height;
    // - dampers x sqrt(gravity_scale): the suspension's damping ratio (damper / (2 sqrt(spring x mass))) is Rush 2's;
    // - the tires' load / gravity_scale: grip is Rush 2's;
    // - on the road, the extra gravity's part along the road is taken out, so hills pull as on any other track:
    //   `extra_downhill` and `extra_uphill` are how much of it is kept where it pulls along the car's travel
    //   (downhill) or against it (uphill), 0 = Rush 2's hills, 1 = the whole heavier world. Its part across the road
    //   (on banked roads) is always taken out. Only the wheels touching the road count (a quarter each), so it goes
    //   as the car leaves the road and the whole extra gravity pulls it down in the air.
    // Springs x 4.6 (the air's gravity, twice as stiff as California Speed's) kicked cars over at ramp feet: Monterey's
    // jump, a 17 degree crease taken at 185 ft/s, threw 6 of 7 into backflips.
    constexpr float gravity_scale = 1.0f + 4 * 0.32f;
    constexpr float extra_downhill = 0.0f;
    constexpr float extra_uphill = 0.0f;

    float f32(uint8_t* rdram, uint32_t at) {
        uint32_t bits = (uint32_t)MEM_W(0, (int32_t)at);
        float f;
        memcpy(&f, &bits, 4);
        return f;
    }
    void put_f32(uint8_t* rdram, uint32_t at, float f) {
        uint32_t bits;
        memcpy(&bits, &f, 4);
        MEM_W(0, (int32_t)at) = (int32_t)bits;
    }

    // Air gravity: extra gravity only while the car is in the air, so on the road every car drives as on any other
    // track. It reaches California Speed's airborne pull (weight x 3.6 more, 4.6 g in all: its jumps' heights) as the
    // car's lowest wheel goes from 0.25 to 1.25 ft above the road, so a wheel lifting in a corner or over a crest
    // adds nothing and a landing doesn't jolt.
    constexpr float air_extra = 3.6f;
    constexpr float air_start = 0.25f, air_full = 1.25f;
    void air_gravity(uint8_t* rdram, uint32_t car) {
        float lowest = 1e9f; // the lowest wheel's height above the road
        for (int i = 0; i < 4; i++) {
            lowest = std::min(lowest, -f32(rdram, car + car_compression + 4 * i));
        }
        float ramp = std::clamp((lowest - air_start) / (air_full - air_start), 0.0f, 1.0f);
        if (ramp <= 0.0f) return;
        float g[3] = { f32(rdram, car + car_gravity), f32(rdram, car + car_gravity + 4),
                       f32(rdram, car + car_gravity + 8) };
        float len = std::sqrt(g[0] * g[0] + g[1] * g[1] + g[2] * g[2]);
        if (len <= 0.0f) return;
        float push = f32(rdram, car + car_weight) * air_extra * ramp;
        for (int k = 0; k < 3; k++) {
            put_f32(rdram, car + car_force + 4 * k, f32(rdram, car + car_force + 4 * k) + g[k] / len * push);
        }
    }

    // California Speed's road hold, not used (full_road_hold): each wheel gives a factor from its compression
    // (func_800A58A4: in the air 0.9 below -0.75, 1.5 below -0.5, else 0.5; on the ground 0.32, or 0.82 - compression
    // past 0.5, which first raises the wheel two places back to at least 0.2), and the car is pushed by weight x their
    // sum (func_8008F23C): into the road under it, or straight down (as gravity) while a wheel is more than 1.0 out of
    // the road. California Speed's x4 once the race is over (byte 0x8014FE0B) isn't kept.
    // The push this frame, from the wheels' compressions (set by car_ground_contact before the wheel forces). With
    // air_only, a wheel on the road gives nothing and one off it its factor less the 0.32 it gave on the road: +0.18
    // as it lifts, +1.18 hanging 0.5-0.75 out (the pull that holds a car over a crest), +0.58 further (2.32 x weight
    // with all four well up, 3.3 g in all).
    constexpr float ground_factor = 0.32f;
    float road_hold_push(uint8_t* rdram, uint32_t car, bool air_only) {
        float factor[4] = {};
        for (int i = 0; i < 4; i++) {
            float c = f32(rdram, car + car_compression + 4 * i);
            if (air_only) {
                factor[i] = c >= 0.0f ? 0.0f : (c < -0.75f ? 0.9f : c < -0.5f ? 1.5f : 0.5f) - ground_factor;
            }
            else if (c < 0.0f) {
                factor[i] = c < -0.75f ? 0.9f : c < -0.5f ? 1.5f : 0.5f;
            }
            else if (c > 0.5f) {
                // California Speed raises wheel (i + 2) % 4's factor as it stands: last frame's for wheels still to come
                // (which they then replace), so only wheels 0 and 1 keep it.
                if (i >= 2 && factor[i - 2] < 0.2f) factor[i - 2] += 0.2f;
                // Negative past 0.82: an upward push California Speed's stiff springs never reach. Rush 2's softer
                // ones go past it on any incline (Monterey's ramp), where it turned the car's support negative
                // (rush2_calspeed_wheel_support) and slammed it down; so no push there.
                factor[i] = std::max(0.0f, 0.5f - c + ground_factor);
            }
            else {
                factor[i] = ground_factor;
            }
        }
        return f32(rdram, car + car_weight) * (factor[0] + factor[1] + factor[2] + factor[3]);
    }

    // Each wheel's support of the body this step (car_wheel_force_loop's 0x34($sp), recorded by
    // rush2_calspeed_wheel_support; the wheel loop runs before the road hold in the same step), per car.
    float wheel_supports[max_race_cars][4];

    // Pushes the car by `push` (lb) as California Speed does: square to the road under it, or straight down once a
    // wheel is more than 1.0 out of the road. `lift` (lb) holds it up, square to the road (Copy's springs).
    void push_car(uint8_t* rdram, uint32_t car, float push, float lift = 0.0f) {
        auto f32 = [&](uint32_t at) -> float { return ::f32(rdram, at); };
        auto put = [&](uint32_t at, float f) { put_f32(rdram, at, f); };
        if (push == 0.0f && lift == 0.0f) return;
        bool lifted = false;
        for (int i = 0; i < 4; i++) lifted |= f32(car + car_compression + 4 * i) < -1.0f;
        float dir[3] = { 0.0f, -1.0f, 0.0f }, down[3] = { 0.0f, -1.0f, 0.0f };
        {
            // California Speed pushes along the car's own down axis, which its stiff springs keep square to the road.
            // Rush 2's softer ones let the body lean a little on them, and a push along a leaning body has a sideways part
            // that Rush 2's tires barely resist at rest: a parked car crept off (about 1 ft/s/s). So on the road the push
            // is square to the road itself: the mean normal of the polygons under the wheels touching it (row 1 of each
            // wheel's world -> polygon matrix, whose local y is the polygon's normal), turned into the car's frame by the
            // car matrix (+0x2CC, row-major, as car_force_sum turns gravity).
            float n[3] = {};
            for (int i = 0; i < 4; i++) {
                if (f32(car + car_compression + 4 * i) < 0.0f) continue;
                uint32_t m = car + car_wheel_polygon + 0x24 * i;
                float w[3] = { f32(m + 12), f32(m + 16), f32(m + 20) };
                float s = w[1] < 0.0f ? -1.0f : 1.0f; // facing up
                for (int k = 0; k < 3; k++) n[k] += w[k] * s;
            }
            float len = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
            if (len > 0.0f) {
                for (int k = 0; k < 3; k++) {
                    uint32_t row = car + car_matrix + 12 * k;
                    dir[k] = -(f32(row) * n[0] + f32(row + 4) * n[1] + f32(row + 8) * n[2]) / len;
                }
            }
        }
        for (int k = 0; k < 3; k++) down[k] = dir[k];
        if (lifted) {
            float g[3] = { f32(car + car_gravity), f32(car + car_gravity + 4), f32(car + car_gravity + 8) };
            float len = std::sqrt(g[0] * g[0] + g[1] * g[1] + g[2] * g[2]);
            if (len <= 0.0f) return;
            for (int k = 0; k < 3; k++) down[k] = g[k] / len;
        }
        for (int k = 0; k < 3; k++) {
            put(car + car_force + 4 * k, f32(car + car_force + 4 * k) + down[k] * push - dir[k] * lift);
        }
    }

    void road_hold(uint8_t* rdram, uint32_t car, bool air_only) {
        float push = road_hold_push(rdram, car, air_only), lift = 0.0f;
        constexpr uint32_t physics_cars = 0x800F5470, physics_car_size = 0x81C;
        uint32_t index = (car - physics_cars) / physics_car_size;
        if (hold == Hold::Copy && !air_only && car >= physics_cars && (car - physics_cars) % physics_car_size == 0 &&
            index < uint32_t(max_race_cars)) {
            // Copy's springs hold the push up: the touching wheels' support (sum S) times push / weight, at the car's
            // center, so the car rides at Rush 2's height while it is on the road and the push shows only as the
            // springs let go. As one force it lifts the car without changing its roll or pitch: the wheel loop, the
            // tires' loads and every weight shift are Rush 2's.
            float weight = f32(rdram, car + car_weight), sum = 0.0f;
            for (int i = 0; i < 4; i++) {
                if (f32(rdram, car + car_compression + 4 * i) >= 0.0f) sum += wheel_supports[index][i];
                wheel_supports[index][i] = 0.0f;
            }
            if (weight > 0.0f) lift = push * sum / weight;
        }
        push_car(rdram, car, push, lift);
    }

    // Shape. What keeps California Speed's cars on its hills is that its hold is there while they are on the road, so
    // over a crest they are already held and follow it; a force that starts once the car is off the road can only pull
    // it back after it has begun to fly. Its ground push can't be copied (on Rush 2's springs, a third as stiff, it
    // sinks the cars and changes their handling), so the push here comes from the road's shape instead: following a
    // crest of curvature k at speed v takes v^2 k of downward acceleration. Pushing only what gravity can't give
    // (v^2 k - g cos(slope)) left the tires at no load over the crest: the car floated, lost grip and spun. The push is
    // the whole of it, weight x v^2 k / g (at most California Speed's 3.6, its pull with all four wheels well off the
    // road), so over a crest the tires keep the load they have on flat road. The curvature is the pitch between the
    // road under the front wheels and under the rear ones (their polygons' normals in the car's frame) over the
    // wheelbase. California Speed's roads are large flat polygons, so a crest is a run of kinks at their edges, and
    // read as it is each kink came as a sharp shove for the moment between the axles crossing it (1 degree at
    // 150 ft/s: 1.4 x weight for 0.06 s) that bounced the springs and unloaded the tires. So the curvature is averaged
    // over shape_curve_time, which keeps each kink's whole push (the turn the car needs over that edge) but spreads
    // it out, the push changes at most shape_rate a second, and kinks under shape_dead count as flat. Flat road, steady grades, dips and
    // corners need nothing, so cars handle there as on any other track. Off the road the push keeps its last value
    // and moves toward 3.6 slowly (shape_air_rate a second), so leaving the road changes nothing at once.
    constexpr float gravity_ftss = 32.2f;
    constexpr float shape_max = 3.6f;      // x weight
    constexpr float shape_air_rate = 2.0f; // x weight per second, off the road
    constexpr float shape_dead = 0.005f;      // rad (0.3 degrees) of pitch between the axles that counts as flat
    constexpr float shape_curve_time = 0.12f; // s, the curvature's average (a polygon edge's pulse lasts wheelbase / v)
    constexpr float shape_rate = 12.0f;       // x weight per second, the most the push changes on the road
    constexpr uint32_t car_dt = 0x634;           // f32 step time (+0x638 its inverse)
    constexpr uint32_t car_velocity = 0x34;      // f32[3] in the car's frame: x sideways, y up, z forward
    constexpr uint32_t desc_wheels = 0x7C;       // descriptor (car +0): f32[4][3] wheel positions, 0-1 front, 2-3 rear
    constexpr uint32_t game_time = 0x80117488;   // f32 s
    struct CarShape {
        float push = 0.0f;  // x weight
        float curve = 0.0f; // 1/ft, the averaged curvature
        float seen = 0.0f; // game time of the last step: a gap or a restart starts again from nothing
    };
    std::map<uint32_t, CarShape> car_shapes;

    // The road's normal under wheel i in the car's frame (row 1 of its polygon's world -> polygon matrix, facing up,
    // turned by the car matrix +0x2CC as car_force_sum turns gravity).
    void road_normal(uint8_t* rdram, uint32_t car, int i, float out[3]) {
        uint32_t m = car + car_wheel_polygon + 0x24 * i;
        float w[3] = { f32(rdram, m + 12), f32(rdram, m + 16), f32(rdram, m + 20) };
        float sign = w[1] < 0.0f ? -1.0f : 1.0f;
        for (int k = 0; k < 3; k++) {
            uint32_t row = car + car_matrix + 12 * k;
            out[k] = sign * (f32(rdram, row) * w[0] + f32(rdram, row + 4) * w[1] + f32(rdram, row + 8) * w[2]);
        }
    }

    void shape_push(uint8_t* rdram, uint32_t car) {
        CarShape& st = car_shapes[car];
        float now = f32(rdram, game_time);
        if (now < st.seen || now > st.seen + 0.5f) st.push = st.curve = 0.0f;
        st.seen = now;
        float dt = std::clamp(f32(rdram, car + car_dt), 0.0f, 0.1f);
        bool down[4];
        for (int i = 0; i < 4; i++) down[i] = f32(rdram, car + car_compression + 4 * i) >= 0.0f;
        bool front = down[0] || down[1], rear = down[2] || down[3];
        float need = -1.0f;
        if (front && rear) {
            // Pitch of the road under each axle (its touching wheels' mean normal), forward positive.
            auto axle_pitch = [&](int a, int b, float n_out[3]) {
                float n[3] = {};
                for (int i : { a, b }) {
                    if (!down[i]) continue;
                    float w[3];
                    road_normal(rdram, car, i, w);
                    for (int k = 0; k < 3; k++) n[k] += w[k];
                }
                for (int k = 0; k < 3; k++) n_out[k] += n[k];
                return std::atan2(n[2], n[1]);
            };
            float n[3] = {};
            float pitch = axle_pitch(0, 1, n) - axle_pitch(2, 3, n); // > 0: the road falls away ahead, a crest
            uint32_t desc = (uint32_t)MEM_W(0, (int32_t)car);
            float wheelbase = 0.5f * (f32(rdram, desc + desc_wheels + 8) + f32(rdram, desc + desc_wheels + 0xC + 8)) -
                              0.5f * (f32(rdram, desc + desc_wheels + 0x18 + 8) + f32(rdram, desc + desc_wheels + 0x24 + 8));
            wheelbase = std::fabs(wheelbase);
            float nlen = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
            if (wheelbase > 1.0f && nlen > 0.0f) {
                float bent = pitch > shape_dead ? pitch - shape_dead : pitch < -shape_dead ? pitch + shape_dead : 0.0f;
                st.curve += (bent / wheelbase - st.curve) * std::min(1.0f, dt / shape_curve_time);
                float v = f32(rdram, car + car_velocity + 8);
                need = std::clamp(v * v * st.curve / gravity_ftss, 0.0f, shape_max);
            }
        }
        if (need >= 0.0f) {
            st.push += std::clamp(need - st.push, -shape_rate * dt, shape_rate * dt);
        }
        else if (!front && !rear) {
            st.push = std::min(shape_max, st.push + shape_air_rate * dt);
        }
        push_car(rdram, car, f32(rdram, car + car_weight) * st.push);
        // RUSH2_ROAD_HOLD_LOG: the player's car (physics car 0) every step: time, wheels down, push (x weight).
        static const bool log = getenv("RUSH2_ROAD_HOLD_LOG") != nullptr;
        if (log && car == 0x800F5470u) {
            printf("[roadhold] t %.2f down %d%d%d%d push %.2f\n", now, down[0], down[1], down[2], down[3], st.push);
        }
    }
}

namespace {
    // A California Speed race: its track is set and the race is in the host slot. The attract demo starts its race
    // without the track select (whose start clears the track), so one on HAWAII, the host slot, right after a
    // California Speed race would still find it set; the demo never races a California Speed track.
    constexpr uint32_t attract_demo = 0x800FAE6C; // u8, nonzero while the attract demo runs
    bool on_calspeed_track(uint8_t* rdram) {
        return raced_track > 0 && MEM_B(0, (int32_t)0x8010C3F0) == rush2::track2049::host_slot &&
               MEM_BU(0, (int32_t)attract_demo) == 0;
    }

    // Hold::Gravity's extra gravity: the road hold's factors (road_hold_push / weight) x the car's gravity (+0x200, the
    // car's frame, turned by car_force_sum), less its part along the road while wheels touch it. In
    // car_physics_forces after car_force_sum, so it is added to the summed force at +0x4.
    void gravity_push(uint8_t* rdram, uint32_t car) {
        float weight = f32(rdram, car + car_weight);
        if (weight <= 0.0f) return;
        float factor = road_hold_push(rdram, car, false) / weight, extra[3];
        for (int k = 0; k < 3; k++) extra[k] = factor * f32(rdram, car + car_gravity + 4 * k);
        // The mean normal (facing up) of the polygons under the touching wheels, in the car's frame (as push_car).
        int touching = 0;
        float wn[3] = {};
        for (int i = 0; i < 4; i++) {
            if (f32(rdram, car + car_compression + 4 * i) < 0.0f) continue;
            uint32_t m = car + car_wheel_polygon + 0x24 * i;
            float w[3] = { f32(rdram, m + 12), f32(rdram, m + 16), f32(rdram, m + 20) };
            float s = w[1] < 0.0f ? -1.0f : 1.0f;
            for (int k = 0; k < 3; k++) wn[k] += w[k] * s;
            touching++;
        }
        float len = std::sqrt(wn[0] * wn[0] + wn[1] * wn[1] + wn[2] * wn[2]);
        if (touching > 0 && len > 0.0f) {
            float n[3];
            for (int k = 0; k < 3; k++) {
                uint32_t row = car + car_matrix + 12 * k;
                n[k] = (f32(rdram, row) * wn[0] + f32(rdram, row + 4) * wn[1] + f32(rdram, row + 8) * wn[2]) / len;
            }
            // The extra gravity along the road, and the direction of travel on it (forward when nearly stopped).
            float into = extra[0] * n[0] + extra[1] * n[1] + extra[2] * n[2];
            float along[3], u[3];
            for (int k = 0; k < 3; k++) along[k] = extra[k] - into * n[k];
            for (int k = 0; k < 3; k++) u[k] = f32(rdram, car + car_velocity + 4 * k);
            if (u[0] * u[0] + u[1] * u[1] + u[2] * u[2] < 1.0f) {
                u[0] = 0.0f;
                u[1] = 0.0f;
                u[2] = 1.0f;
            }
            float un = u[0] * n[0] + u[1] * n[1] + u[2] * n[2];
            for (int k = 0; k < 3; k++) u[k] -= un * n[k];
            float ulen = std::sqrt(u[0] * u[0] + u[1] * u[1] + u[2] * u[2]);
            float travel = 0.0f;
            if (ulen > 0.0f) {
                for (int k = 0; k < 3; k++) u[k] /= ulen;
                travel = along[0] * u[0] + along[1] * u[1] + along[2] * u[2];
            }
            float keep = travel > 0.0f ? extra_downhill : extra_uphill;
            float share = touching / 4.0f;
            for (int k = 0; k < 3; k++) extra[k] -= share * (along[k] - (ulen > 0.0f ? keep * travel * u[k] : 0.0f));
        }
        for (int k = 0; k < 3; k++) {
            put_f32(rdram, car + car_force + 4 * k, f32(rdram, car + car_force + 4 * k) + extra[k]);
        }
    }
}

extern "C" void rush2_calspeed_road_hold(uint8_t* rdram, recomp_context* ctx) {
    if (!road_hold_on || !on_calspeed_track(rdram)) {
        return;
    }
    uint32_t car = (uint32_t)ctx->r16;
    if (hold == Hold::Gravity) {
        gravity_push(rdram, car);
    }
    else if (hold == Hold::Copy) {
        road_hold(rdram, car, false);
    }
    else if (hold == Hold::Shape) {
        shape_push(rdram, car);
    }
    else if (hold == Hold::AirGravity) {
        air_gravity(rdram, car);
    }
    else {
        road_hold(rdram, car, hold == Hold::AirPull);
    }
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
    config.add_bool_option(
        road_hold_option_id,
        "California Speed Road Hold",
        "Keeps cars on California Speed's hills as California Speed does: over a crest every car is held to the road "
        "instead of flying off it. Flat road, slopes and corners drive as on any other track. Off, its tracks have "
        "Rush 2's own gravity. Takes effect at once.",
        true
    );
    config.add_option_change_callback(road_hold_option_id,
        [](recomp::config::ConfigValueVariant cur_value, recomp::config::ConfigValueVariant, recomp::config::OptionChangeContext) {
            road_hold_on = std::get<bool>(cur_value);
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
    page->add_option(rush2::wings::games_config(), road_hold_option_id);
    refresh = [note = heading.note, shown = rush2::calspeed::rom_available()]() mutable {
        if (rush2::calspeed::rom_available() != shown) {
            shown = !shown;
            note->set_text(rom_status_text());
        }
    };
}

// Runs after rush2::wings::load_config(), which loads the option.
void rush2::calspeed::load_config() {
    road_hold_on = std::get<bool>(rush2::wings::games_config().get_option_value(road_hold_option_id));
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

// Copy: the springs hold the road hold's push up. In car_wheel_force_loop after car_tire_model, at 0x8006ADFC ($a0 =
// the car, $t0 = the wheel): the value at 0x34($sp) is the wheel's support of the body (the y of the force vector
// turned into the car's frame at 0x8006AE10). With all four wheels on the road it is only recorded, and road_hold
// adds push x (sum of the supports) / weight at the car's center, square to the road, so nothing in the wheel loop
// changes; with a wheel off the road each support is multiplied by (weight + push) / weight in place instead.
// Tried and set aside: multiplying each wheel's 0x34 by (weight + ground push) / weight. The same lift in all, but per
// wheel it made the springs 2.3 times as stiff in roll and pitch, so the tires' loads shifted less in a swerve than on
// any other track; and hooked at 0x8006AD18 the scaled support went into the tire load's cap (cut to the weight when
// 0x34 is above it), so a loaded wheel's tire jumped to the full weight and cars let go swerving downhill.
// The ground push is the wheels on the road's part (0.32 each, less past 0.5 compression): lifting by the whole push,
// a lifted wheel's 1.5 included, made the springs up to 7 times as strong and bounced and rolled the cars.
extern "C" void rush2_calspeed_wheel_support(uint8_t* rdram, recomp_context* ctx) {
    if (hold != Hold::Copy || !road_hold_on || !on_calspeed_track(rdram)) {
        return;
    }
    constexpr uint32_t physics_cars = 0x800F5470, physics_car_size = 0x81C;
    constexpr uint32_t car_wheels = 0x424, wheel_size = 0x5C;
    uint32_t car = (uint32_t)ctx->r4, wheel = (uint32_t)ctx->r8 - car - car_wheels;
    if (car < physics_cars || (car - physics_cars) % physics_car_size != 0 || wheel % wheel_size != 0 ||
        wheel / wheel_size >= 4 || (car - physics_cars) / physics_car_size >= uint32_t(max_race_cars)) {
        return;
    }
    uint32_t at = (uint32_t)ctx->r29 + 0x34;
    float& support = wheel_supports[(car - physics_cars) / physics_car_size][wheel / wheel_size];
    support = f32(rdram, at);
    bool all_down = true;
    for (int i = 0; i < 4; i++) all_down &= f32(rdram, car + car_compression + 4 * i) >= 0.0f;
    if (all_down) {
        return;
    }
    // A wheel off the road (a landing, or a wheel lifted in a swerve): this wheel's support is lifted where it is,
    // so the wheels that touch first push their side up and level the car (at the center the lift left a car that
    // landed on two wheels tipped: Santa Cruz's chute at (-17320, -610, 4890) rolled cars half again as often).
    float push = 0.0f; // the ground push / weight
    for (int i = 0; i < 4; i++) {
        float c = f32(rdram, car + car_compression + 4 * i);
        if (c >= 0.0f) push += c > 0.5f ? std::max(0.0f, 0.5f - c + ground_factor) : ground_factor;
    }
    put_f32(rdram, at, support * (1.0f + push));
    support = 0.0f;
}

// The damper on California Speed's tracks. car_wheel_force_loop's load is (anti-roll + compression x spring -
// v x damper) x a time-step scale, with v the wheel's velocity along the normal of the polygon under it
// (car_wheel_frame_velocity_8006AE3C). California Speed's roads are large flat polygons: as a wheel crosses onto one
// at another slope, v jumps at once by speed x the change of slope, and the damper hits the car with all of it in one
// frame (a 3 degree edge at 190 ft/s is 10 ft/s: a kick, or a wheel dropped for a frame, the slight lifts that let
// cars spin out). Monterey's 1.6 ft curb at (-2870, -55, 8765), which its lanes cross at 170 mph, has a 29 degree
// face 2.9 ft deep: a rear wheel that landed on it for one frame read v = -85 ft/s while its spring was extending,
// took 12.5 x the car's weight and flipped the car over backward.
// So v may move only `damper_jump` ft/s per second past the larger of last frame's v and the spring's own rate (its
// compression's change since then) in one frame. On smooth road v changes far less than that, and nothing changes;
// at an edge or a step only the frame it jumps is cut, and the next frame's v is the game's again. A wheel that was
// off the road last frame (a landing) keeps v. v stays the damper's input: the spring's rate in its place, a frame
// behind, made the stiffer dampers (type 39's) oscillate, and holding v back over several frames (a slew) left every
// car bouncier: Monterey's computer cars rolled 28 times, against 13 with the one frame cut and 41 on stock gravity.
// Hooked at 0x8006AD10, after the load's product ($f2) and before it is compared with 0 and stored: $a0 = the car,
// $a1 = the wheel's velocity (+4 = v), $t0 = its wheel struct (car + 0x424 + 0x5C x wheel), the loop's own
// arguments at 0x78($sp): compression, the other wheel's on its axle, spring, anti-roll, damper when v < 0 and when not.
namespace {
    constexpr float damper_jump = 200.0f; // ft/s per second: 6.7 ft/s in a 1/30 s frame
    struct WheelRate {
        float c[2] = {};    // compression: last frame's, this frame's
        float v[2] = {};    // v as the game gave it: last frame's, this frame's
        float t[2] = { -1.0f, -1.0f };
    };
    WheelRate wheel_rates[max_race_cars][4];
}

extern "C" void rush2_calspeed_wheel_damper(uint8_t* rdram, recomp_context* ctx) {
    if (!road_hold_on || !on_calspeed_track(rdram)) {
        return;
    }
    constexpr uint32_t physics_cars = 0x800F5470, physics_car_size = 0x81C;
    constexpr uint32_t car_wheels = 0x424, wheel_size = 0x5C;
    constexpr uint32_t car_inv_dt = 0x63C;
    constexpr uint32_t step_scale = 0x800CF778; // f32[2]: the load is multiplied by 1/dt x (1/dt x [0] - [1])
    constexpr uint32_t game_time = 0x80117488;
    uint32_t car = (uint32_t)ctx->r4, sp = (uint32_t)ctx->r29;
    uint32_t wheel = (uint32_t)ctx->r8 - car - car_wheels;
    if (car < physics_cars || (car - physics_cars) % physics_car_size != 0 || wheel % wheel_size != 0 ||
        wheel / wheel_size >= 4 || (car - physics_cars) / physics_car_size >= uint32_t(max_race_cars)) {
        return;
    }
    WheelRate& w = wheel_rates[(car - physics_cars) / physics_car_size][wheel / wheel_size];
    float c = f32(rdram, sp + 0x78), now = f32(rdram, game_time);
    float v = f32(rdram, (uint32_t)ctx->r5 + 4);
    if (now != w.t[1]) {
        w.c[0] = w.c[1];
        w.v[0] = w.v[1];
        w.t[0] = w.t[1];
        w.t[1] = now;
    }
    w.c[1] = c;
    w.v[1] = v;
    float gap = now - w.t[0];
    if (c <= 0.0f || c > 10.0f) {
        return;
    }
    bool clamped = false;
    if (w.c[0] > 0.0f && gap > 0.0f && gap <= 0.25f) {
        float rate = -(c - w.c[0]) / gap;
        float limit = std::max(std::fabs(w.v[0]), std::fabs(rate)) + damper_jump * gap;
        clamped = std::fabs(v) > limit;
        v = std::clamp(v, -limit, limit);
    }
    if (!clamped && hold != Hold::Gravity) {
        return;
    }
    // Hold::Gravity: springs and anti-roll x gravity_scale, dampers x its square root (the load at 0x8006AC8C-0x8006AD0C
    // for 0 < c <= 10; anti-roll only while both compressions are above 0).
    float stiff = hold == Hold::Gravity ? gravity_scale : 1.0f, damp = std::sqrt(stiff);
    float c0 = f32(rdram, sp + 0x7C), spring = f32(rdram, sp + 0x80), anti_roll = f32(rdram, sp + 0x84);
    float damper = f32(rdram, sp + (v < 0.0f ? 0x88 : 0x8C));
    float inv_dt = f32(rdram, car + car_inv_dt);
    float scale = inv_dt * (inv_dt * f32(rdram, step_scale) - f32(rdram, step_scale + 4));
    float roll = c0 > 0.0f ? (c - c0) * anti_roll : 0.0f;
    ctx->f2.fl = ((roll + c * spring) * stiff - v * damper * damp) * scale;
}

// Hold::Gravity: the tire's load as on any other track. In car_wheel_force_loop at 0x8006AD4C, as the load goes to
// car_tire_model ($a2; $a0 = the car): the springs' load (0x34($sp), x gravity_scale with the springs) /
// gravity_scale, capped at the car's weight (+0x5AC) as the game caps it (0x8006AD30-0x8006AD44).
extern "C" void rush2_calspeed_tire_load(uint8_t* rdram, recomp_context* ctx) {
    if (hold != Hold::Gravity || !road_hold_on || !on_calspeed_track(rdram)) {
        return;
    }
    uint32_t car = (uint32_t)ctx->r4;
    float load = std::min(f32(rdram, (uint32_t)ctx->r29 + 0x34) / gravity_scale, f32(rdram, car + car_weight));
    uint32_t bits;
    memcpy(&bits, &load, 4);
    ctx->r6 = (uint64_t)(int64_t)(int32_t)bits;
}

// Rush 2's fall floor: race_stuck_wrongway_update_8008CDA4 puts back a car whose y is below -190 (0x8008CF20, $f16;
// SF Rush's tracks use -290, rush2_track1_fall_floor). California Speed's roads go lower (Silicon Valley's to -306),
// so its cars were put back every few seconds there, onto a road still under the floor. On its tracks the floor is
// `floor_depth` below the racing line's lowest point: its ground runs up to about 200 ft below the roads, and a car
// that falls off is wrecked sooner by rush2_calspeed_car_frame (60 ft under the racing line for 2 s).
extern "C" void rush2_calspeed_fall_floor(uint8_t* rdram, recomp_context* ctx) {
    constexpr float floor_depth = 300.0f;
    if (race_points.empty() || !on_calspeed_track(rdram)) {
        return;
    }
    float lowest = 1e30f;
    for (const auto& p : race_points) lowest = std::min(lowest, p[1]);
    ctx->f16.fl = std::min(ctx->f16.fl, lowest - floor_depth);
}
