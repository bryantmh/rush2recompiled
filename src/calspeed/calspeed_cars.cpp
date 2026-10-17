// California Speed's cars as Rush 2 car types 36-56 (include/calspeed.h, docs/calspeed_research.md#cars).
//
// California Speed runs Rush 2's car code (its car struct is Rush 2's with the fields from +0x5B0 shifted by 0x64), so
// its cars convert to Rush 2's data almost one to one:
// - Model: asset 41 + n is a Rush 2 container holding only <PREFIX>FRAME1 (two LODs) and its textures. It becomes a car
//   like a converted 2049 one (rush2::track2049::convert_body_car): the body as CSnFRAME1, Rush 2's other parts empty,
//   dented by panel damage (src/rush2049/car2049.cpp), served as asset 0x80 + n.
// - Paint: every car's textures use CARPALETTE (asset 25), which California Speed repaints with the car's two colors
//   (func_800BAE2C); car2049.cpp paints it the same way with MAIN and ACCENT.
// - Physics: its 0xE0-byte descriptors are Rush 2's 0xEC-byte layout with a few fields missing (build_descriptor), and
//   its per-type tables have Rush 2's layout and meaning (steering, inertia, TIRES 4 = S1 D1 for every car).
// The converted containers are kept on disk (track_cache blobs keyed on the ROM and the build). The per-player table
// rows (put_word / put_byte) are the car select's starting values; the record's own options replace them there.

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "recomp.h"

#include "calspeed.h"
#include "car2049.h"
#include "car_types.h"
#include "paint_colors.h"
#include "track2049_convert.h"
#include "track_cache.h"

namespace {
    namespace ct = rush2::car_types;
    using rush2::calspeed::car_count;
    static_assert(car_count == ct::count_calspeed);

    constexpr int first_car_asset = 41; // California Speed asset 41 + n: car n's container
    constexpr int palette_asset = 25;   // CARPALETTE
    constexpr uint32_t cache_version = 1;
    // The model names in each container (<prefix>FRAME1), from its asset.
    constexpr const char* source_prefixes[car_count] = { "Z3", "CAMERO", "FERRARI", "DIABLO", "MANO", "COPCAR", "M2",
        "PROWL", "SUPER", "BUG", "CIVIC", "GOLF", "DRONE", "FORK", "CADDY", "SLED", "BAJA", "OLTRUCK", "WIN", "TRUCK",
        "DOZER" };

    // California Speed's per-car tables (main code, 21 entries each; docs/calspeed_research.md#cars).
    constexpr uint32_t cs_steer = 0x8007CF20;        // f32 steer force base (Rush 2 0x800C06B4)
    constexpr uint32_t cs_yaw_damping = 0x8007CF74;  // f32 (Rush 2 0x800C070C)
    constexpr uint32_t cs_desc_index = 0x8007D01C;   // u8 type -> descriptor (Rush 2 0x800C07E8)
    constexpr uint32_t cs_preload = 0x8007D034;      // f32 (Rush 2 0x800C0800)
    constexpr uint32_t cs_mass = 0x8007D088;         // f32, x cs_mass_scale (func_8009A8E0)
    constexpr uint32_t cs_inertia = 0x8007D0DC;      // f32 pitch/roll inertia (Rush 2 0x800C08B0)
    constexpr uint32_t cs_drive = 0x8007D2C4;        // u8 drive flags (Rush 2 0x800C0D14)
    constexpr uint32_t cs_tires = 0x8007D304;        // u8 handling = Rush 2's TIRES: the steer and yaw adds' index
    constexpr uint32_t cs_mass_scale = 0x8007D47C;   // f32 (1.0)
    constexpr uint32_t cs_desc_ptrs = 0x800D6418;    // descriptor pointers (18)
    constexpr uint32_t cs_boxes = 0x800D60E0;        // 16 bytes per type: {front, rear, half width, height}
    constexpr uint32_t cs_front_curve = 0x800D50F0;  // tire curves, 6 floats each (radius, spring, damper,
    constexpr uint32_t cs_rear_curve = 0x800D5108;   // cornering stiffness, grip, inverse wheel inertia)
    // Rush 2's crash threshold (car +0x644, table 0x800C090C: a total force above it wrecks the car): California Speed's
    // table at 0x8007D134 holds 390000 for every car (its car init doesn't read it).
    constexpr float crash_threshold = 390000.0f;
    constexpr uint32_t desc_size = 0xE0;

    // Each car's default colors (0x800DC0D0 and 0x800DC124: RGBA words; func_8005E5C0 makes a new player record's
    // colors from them, func_800A6F88 the drones') and TIRE RIMS (row 0 of 0x8007D3BC). Constants: there is one
    // California Speed (USA) ROM, and the side slots made before it is loaded need them.
    constexpr uint32_t default_rgb[car_count][2] = {
        { 0x0000FF, 0x808080 }, { 0x008000, 0x000000 }, { 0xFF0000, 0x000000 }, { 0xFF8000, 0x000000 },
        { 0xC8C800, 0x000000 }, { 0x0000FF, 0xFFFFFF }, { 0x0000FF, 0x000000 }, { 0x008000, 0x000000 },
        { 0x0000FF, 0xFFFFFF }, { 0x800080, 0xFFFFFF }, { 0x000096, 0xFFFFFF }, { 0x008000, 0x000000 },
        { 0xB2B2C8, 0x000000 }, { 0xFFFF00, 0x000000 }, { 0x8080FF, 0x000000 }, { 0x800080, 0xFFFF00 },
        { 0xFF0000, 0xFFFFFF }, { 0x009673, 0x000000 }, { 0x0000FF, 0x000000 }, { 0x000000, 0x000000 },
        { 0xC8C800, 0x000000 },
    };
    constexpr uint8_t default_rims[car_count] = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 2, 1, 0, 8, 9, 6, 1, 3, 4, 5, 6, 2 };
    // Rush 2's ENGINE (its engine sound, 0-9) for each car, from the Rush 2 car most like it (California Speed has its
    // own engine sounds, not ported): sports cars the BMW's or the Vette's, the trucks the SUV's, the small cars the
    // VW's.
    constexpr int8_t engines[car_count] = { 9, 5, 0, 0, 2, 2, 5, 0, 1, 3, 8, 3, 3, 3, 0, 0, 6, 6, 9, 9, 9 };
    // Rush 2's SUSPENSION has no California Speed counterpart (its suspension code reads only descriptor +0x50):
    // NORMAL, so the car select can move it both ways.
    constexpr int8_t suspension = 1;

    std::atomic_bool cars_option = true;
    std::atomic<uint32_t> drones = (uint32_t)rush2::car2049::DroneCars::AllTracks;
    const std::vector<uint8_t>* loaded_from = nullptr; // the ROM the cars were last built from
    std::shared_ptr<const std::vector<uint8_t>> loaded_rom;
    std::atomic_bool cars_loaded = false; // read by the UI thread (the AI Opponents list)

    uint32_t be32(const std::vector<uint8_t>& d, size_t o) {
        return (uint32_t(d[o]) << 24) | (uint32_t(d[o + 1]) << 16) | (uint32_t(d[o + 2]) << 8) | d[o + 3];
    }
    uint8_t main_byte(const std::vector<uint8_t>& rom, uint32_t vram) {
        return uint8_t(rush2::calspeed::main_word(rom, vram & ~3u) >> (24 - 8 * (vram & 3)));
    }
    float f32_of(uint32_t bits) {
        float f;
        memcpy(&f, &bits, 4);
        return f;
    }
    uint32_t fbits(float f) {
        uint32_t v;
        memcpy(&v, &f, 4);
        return v;
    }

    // The nearest of Rush 2's 32 paint colors to 0xRRGGBB (perceptual weights).
    int nearest_color(uint32_t rgb) {
        int best = 0;
        float best_d = 1e30f;
        float r = float(rgb >> 16), g = float((rgb >> 8) & 0xFF), b = float(rgb & 0xFF);
        for (int i = 0; i < rush2::paint_colors::rush2_colors; i++) {
            uint32_t c = rush2::paint_colors::rgba(i);
            float dr = float(c >> 24) - r, dg = float((c >> 16) & 0xFF) - g, db = float((c >> 8) & 0xFF) - b;
            float d = 0.3f * dr * dr + 0.59f * dg * dg + 0.11f * db * db;
            if (d < best_d) {
                best_d = d;
                best = i;
            }
        }
        return best;
    }

    // Car n's part name prefix and its textures' (unique across the cars: several share WINDOW and UNDRSDE2).
    std::string part_prefix(int n) {
        return "CS" + std::to_string(n + 1);
    }

    // Car n's container, converted (or from the disk cache).
    bool load_car(const std::vector<uint8_t>& rom, int n, std::vector<uint8_t>& car, std::string& error) {
        uint64_t key = rush2::calspeed::rom_source(rom) ^ (uint64_t(cache_version) << 56);
        std::string cache_name = "calspeed_car_" + std::to_string(n + 1);
        if (rush2::track_cache::load_blob(cache_name, key, car)) {
            return true;
        }
        std::vector<uint8_t> source;
        if (!rush2::calspeed::read_asset(rom, first_car_asset + n, source)) {
            error = "can't read asset " + std::to_string(first_car_asset + n);
            return false;
        }
        if (!rush2::track2049::convert_body_car(source, std::string(source_prefixes[n]) + "FRAME1", part_prefix(n),
                                                part_prefix(n) + "_", car, error)) {
            return false;
        }
        rush2::track_cache::save_blob(cache_name, key, car);
        return true;
    }

    // CARPALETTE's 256 RGBA5551 entries (big-endian), from asset 25's palette record (name[16], first, last, data).
    bool load_palette(const std::vector<uint8_t>& rom, std::vector<uint8_t>& out) {
        std::vector<uint8_t> asset;
        if (!rush2::calspeed::read_asset(rom, palette_asset, asset) || asset.size() < 0x28) return false;
        uint32_t records = be32(asset, 12);
        if (be32(asset, 24) < 1 || records + 0x18 > asset.size()) return false;
        uint32_t data = be32(asset, records + 0x14);
        if (data + 512 > asset.size()) return false;
        out.assign(asset.begin() + data, asset.begin() + data + 512);
        return true;
    }

    // Car n's descriptor as Rush 2's 0xEC-byte layout (docs/calspeed_research.md#cars). California Speed's lacks
    // Rush 2's +0x54-+0x5C (its suspension code reads only +0x50), the rear lateral grip (+0x78) and the reverse gear;
    // those come from the Pickup (1.0 rear grip: the grip factors are in the car's tire curves). Its two grip factors
    // (+0x6C, +0x70; func_800B8CE8 scales a tire's two force components by them) are folded into its curves' grip.
    void build_descriptor(uint8_t* rdram, const std::vector<uint8_t>& rom, int n, uint32_t out) {
        auto w = [&](uint32_t vram) { return rush2::calspeed::main_word(rom, vram); };
        uint32_t d = w(cs_desc_ptrs + 4 * main_byte(rom, cs_desc_index + n));
        uint32_t pickup = (uint32_t)MEM_W(0, (int32_t)(0x800C07BC + MEM_BU(0, (int32_t)0x800C07E8) * 4));
        auto put = [&](uint32_t o, uint32_t v) { MEM_W(0, (int32_t)(out + o)) = (int32_t)v; };
        auto copy = [&](uint32_t to, uint32_t from, int words) {
            for (int i = 0; i < words; i++) put(to + 4 * i, w(d + from + 4 * i));
        };
        copy(0x00, 0x00, 0x54 / 4);                                   // inertia, suspension
        for (uint32_t o : { 0x54u, 0x58u, 0x5Cu, 0xC8u }) put(o, (uint32_t)MEM_W(0, (int32_t)(pickup + o)));
        copy(0x60, 0x54, 2);                                          // drag, rolling resistance
        // Tire curves: copies of California Speed's front and rear ones with the car's grip factor.
        uint32_t curves = ct::calspeed_tire_curves + n * 0x30;
        float grip = f32_of(w(d + 0x70));
        for (int c = 0; c < 2; c++) {
            uint32_t from = c == 0 ? cs_front_curve : cs_rear_curve;
            for (int i = 0; i < 6; i++) {
                uint32_t v = w(from + 4 * i);
                if (i == 4) v = fbits(f32_of(v) * grip);
                MEM_W(0, (int32_t)(curves + c * 0x18 + i * 4)) = (int32_t)v;
            }
        }
        for (int i = 0; i < 4; i++) put(0x68 + 4 * i, w(d + 0x5C + 4 * i) == cs_front_curve ? curves : curves + 0x18);
        put(0x78, fbits(1.0f));
        copy(0x7C, 0x74, 12);                                         // wheel positions
        copy(0xAC, 0xA4, 3);                                          // drivetrain inertia, final drive, clutch
        copy(0xB8, 0xB0, 4);                                          // torque scales
        copy(0xCC, 0xC0, 5);                                          // neutral, gears 1-4
        put(0xE0, w(d + 0xD4));                                       // top gear (s16), pad
        copy(0xE4, 0xD8, 2);                                          // shift thresholds
    }

    // Car n's physics: its descriptor, per-type table entries (every row) and box.
    void build_physics(uint8_t* rdram, const std::vector<uint8_t>& rom, int n) {
        int type = ct::first_calspeed + n;
        int desc = 11 + ct::count_2049 + n; // after Rush 2's 11 and the 2049 cars' descriptors
        uint32_t out = ct::descs + (ct::count_2049 + n) * ct::desc_size;
        build_descriptor(rdram, rom, n, out);
        MEM_W(0, (int32_t)(ct::desc_ptrs + desc * 4)) = (int32_t)out;
        auto w = [&](uint32_t vram) { return rush2::calspeed::main_word(rom, vram); };
        auto put_word = [&](uint32_t table, uint32_t v) {
            for (int row = 0; row < ct::player_rows; row++) MEM_W(0, (int32_t)ct::entry(table, row, type)) = (int32_t)v;
        };
        auto put_byte = [&](uint32_t table, int v) {
            for (int row = 0; row < ct::player_rows; row++) MEM_B(0, (int32_t)ct::entry(table, row, type)) = (int8_t)v;
        };
        auto put_one = [&](uint32_t table, uint32_t v) { MEM_W(0, (int32_t)ct::entry(table, 0, type)) = (int32_t)v; };
        MEM_B(0, (int32_t)ct::entry(ct::desc_index, 0, type)) = (int8_t)desc;
        put_one(ct::steer_base, w(cs_steer + 4 * n));
        put_one(ct::yaw_damping, w(cs_yaw_damping + 4 * n));
        put_one(ct::preload, w(cs_preload + 4 * n));
        put_one(ct::mass, fbits(f32_of(w(cs_mass + 4 * n)) * f32_of(w(cs_mass_scale))));
        put_one(ct::inertia, w(cs_inertia + 4 * n));
        put_one(ct::yaw_inertia, fbits(crash_threshold));
        put_word(ct::weight, fbits(0.5f));
        put_word(ct::front_wheel_scale, fbits(1.0f));
        put_word(ct::rear_wheel_scale, fbits(1.0f));
        put_byte(ct::engine, engines[n]);
        put_byte(ct::torque, 2);                                       // HIGH, as California Speed's map index
        put_byte(ct::drive, main_byte(rom, cs_drive + n));
        put_byte(ct::suspension, suspension);
        put_byte(ct::tires, main_byte(rom, cs_tires + n));
        rush2::calspeed::CarDefaults def = rush2::calspeed::car_defaults(n);
        put_byte(ct::rims, def.rims);
        put_byte(ct::main_color, def.main);
        put_byte(ct::accent_color, def.accent);
        put_byte(ct::stripe, 0);
        put_byte(ct::stripe_color, 0);
        for (int i = 0; i < 4; i++) {
            MEM_W(0, (int32_t)(ct::boxes + type * 16 + i * 4)) = (int32_t)w(cs_boxes + 16 * n + 4 * i);
        }
    }

    void load_cars(uint8_t* rdram, const std::shared_ptr<const std::vector<uint8_t>>& rom) {
        loaded_rom = rom;
        loaded_from = rom.get();
        cars_loaded = false;
        if (rom == nullptr) {
            return;
        }
        std::vector<uint8_t> palette;
        if (!load_palette(*rom, palette)) {
            fprintf(stderr, "[CalSpeed] Couldn't read CARPALETTE: no California Speed cars\n");
            return;
        }
        int served = 0;
        for (int n = 0; n < car_count; n++) {
            std::vector<uint8_t> car;
            std::string error;
            if (!load_car(*rom, n, car, error)) {
                fprintf(stderr, "[CalSpeed] Couldn't convert car %d (%s): %s\n", n + 1, source_prefixes[n], error.c_str());
                continue;
            }
            build_physics(rdram, *rom, n);
            rush2::car2049::set_calspeed_car(rdram, n, std::move(car), part_prefix(n), palette);
            served++;
        }
        std::vector<std::string> logos(rush2::calspeed::car_names, rush2::calspeed::car_names + car_count);
        rush2::car2049::set_calspeed_logos(rdram, logos);
        cars_loaded = served == car_count;
        printf("[CalSpeed] %d cars\n", served);
    }
}

rush2::calspeed::CarDefaults rush2::calspeed::car_defaults(int n) {
    CarDefaults d;
    if (n < 0 || n >= car_count) return d;
    d.main = nearest_color(default_rgb[n][0]);
    d.accent = nearest_color(default_rgb[n][1]);
    d.rims = default_rims[n];
    return d;
}

void rush2::calspeed::set_cars_option(bool enabled) {
    cars_option = enabled;
}

bool rush2::calspeed::cars_available() {
    return cars_option.load(std::memory_order_relaxed) && cars_loaded && rom_available();
}

void rush2::calspeed::set_drone_cars(rush2::car2049::DroneCars mode) {
    drones = (uint32_t)mode;
}

rush2::car2049::DroneCars rush2::calspeed::drone_cars() {
    return (rush2::car2049::DroneCars)drones.load();
}

void rush2::calspeed::init_cars(uint8_t* rdram) {
    load_cars(rdram, get_rom());
}

// Once per frame: a ROM chosen in the Games tab since boot gets its cars while the game is in the menus or the track
// select (game state 0-1), when no car is loaded.
void rush2::calspeed::check_cars(uint8_t* rdram) {
    std::shared_ptr<const std::vector<uint8_t>> rom = get_rom();
    if (rom.get() == loaded_from) {
        return;
    }
    int32_t state = MEM_W(0, (int32_t)0x8010C0D0);
    if (state < 0 || state > 1) {
        return;
    }
    load_cars(rdram, rom);
}
