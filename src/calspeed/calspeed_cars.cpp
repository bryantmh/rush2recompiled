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
//   its per-type tables have Rush 2's layout and meaning (steering, inertia, TIRES 4 = S1 D1 for every car). Its values
//   are tuned against a road hold Rush 2's physics lacks (far stiffer springs and tires), so they are mapped onto Rush
//   2's roster as the 2049 cars' are (Rebalance): the California Speed roster's mean lands on Rush 2's and each car
//   keeps its place among California Speed's. The road hold itself belongs to the tracks (calspeed_rom.cpp).
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
#include <utility>
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
    // Each car's front and rear wheel model scale (0x8007D1DC / 0x8007D230, one row: its car preview func_800BB3E8 and
    // race drawing scale the wheel nodes by them, as Rush 2's 0x800C0A6C / 0x800C0B74): the Golf Cart's small wheels
    // 0.65, the Semi's and Dozer's big ones up to 1.5.
    constexpr float default_wheel_scale[2][car_count] = {
        { 0.9f, 0.9f, 0.95f, 0.95f, 0.95f, 0.95f, 1.15f, 0.95f, 0.95f, 1.0f, 0.95f, 0.65f, 0.87f, 0.95f, 0.95f, 0.95f,
          1.35f, 1.0f, 0.85f, 1.4f, 1.2f },
        { 0.95f, 1.05f, 1.05f, 1.05f, 1.05f, 1.0f, 1.15f, 1.05f, 1.05f, 1.0f, 0.95f, 0.65f, 0.87f, 0.9f, 0.96f, 1.1f,
          1.35f, 1.0f, 1.0f, 1.4f, 1.5f },
    };
    // Rush 2's ENGINE (its engine sound, 0-9) for each car, from the Rush 2 car most like it: sports cars the BMW's or
    // the Vette's, the trucks the SUV's, the small cars the VW's. Only heard if California Speed's own engines (played
    // by src/car_engines.cpp) can't be read from its ROM.
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

    std::string car_cache_name(int n) {
        return "calspeed_car_" + std::to_string(n + 1);
    }

    // Car n's container, converted (or from the disk cache). Any thread: rush2::calspeed::prepare_cars makes them ahead
    // of the boot, which then waits on the entry's claim instead of converting it again.
    bool load_car(const std::vector<uint8_t>& rom, int n, std::vector<uint8_t>& car, std::string& error) {
        uint64_t key = rush2::calspeed::rom_source(rom) ^ (uint64_t(cache_version) << 56);
        std::string cache_name = car_cache_name(n);
        auto claim = rush2::track_cache::claim(rush2::track_cache::blob_entry(cache_name));
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
    // native: for California Speed's physics (rush2::calspeed::native_car), its curves at `curves` as California Speed
    // has them (the grip factors applied per wheel instead).
    void build_descriptor(uint8_t* rdram, const std::vector<uint8_t>& rom, int n, uint32_t out,
                          uint32_t curves = 0, bool native = false) {
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
        if (curves == 0) curves = ct::calspeed_tire_curves + n * 0x30;
        float grip = native ? 1.0f : f32_of(w(d + 0x70));
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

    uint32_t descriptor_of(int n) {
        return ct::descs + (ct::count_2049 + n) * ct::desc_size;
    }

    // The mapping onto Rush 2's roster (docs/calspeed_research.md#rebalance): each value is scaled by Rush 2's mean over
    // California Speed's (Rush 2's 21 cars, all but the secret ROCKET, as the 2049 cars' mapping counts them). Where
    // California Speed gives every car the same value (steering, yaw damping, preload, tire curves, torque scales, gears)
    // that becomes Rush 2's mean; where its cars differ (springs and dampers, mass, inertia) its mean car lands on Rush
    // 2's. The per-car grip factors stay as they are, on Rush 2's curves. The handling row is Rush 2's mean, as for the 2049 cars. The fields both rosters share
    // (inertia constants, drag, final drive, clutch, shift points) are the same in both games and left alone.
    struct Rebalance {
        float desc[ct::desc_size / 4];
        float curve[2][6];
        float roll[2] = {}; // Rush 2's grip / half track (front, rear): the most a car's grip may be per foot of track
        float steer = 1.0f, yaw = 1.0f, preload = 1.0f, mass = 1.0f, inertia = 1.0f;
        float steer_means[2] = {}; // the steer base's mean, Rush 2's and California Speed's (native_values)
        int handling = 5;
    };
    Rebalance mapping; // load_cars' mapping, undone for the other games' cars by native_values
    // The Rush 2 descriptor ranges that are scaled: springs and dampers, torque scales (gears 1, 2, 3+, common map) and
    // gears 1-4.
    constexpr std::pair<uint32_t, uint32_t> scaled_fields[] = { { 0x18, 0x50 }, { 0xB8, 0xC8 }, { 0xD0, 0xE0 } };

    // Call once build_descriptor has made every car's descriptor (unscaled).
    Rebalance rebalance(uint8_t* rdram, const std::vector<uint8_t>& rom) {
        constexpr int secret_rocket = 20;
        constexpr uint32_t r2_desc_ptrs = 0x800C07BC, r2_desc_index = 0x800C07E8; // the ROM's tables, kept as they were
        constexpr uint32_t r2_steer = 0x800C06B4, r2_yaw_damping = 0x800C070C, r2_preload = 0x800C0800,
                           r2_mass = 0x800C0858, r2_inertia = 0x800C08B0, r2_handling = 0x800C0DAC;
        auto at = [&](uint32_t a) { return f32_of((uint32_t)MEM_W(0, (int32_t)a)); };
        auto w = [&](uint32_t vram) { return f32_of(rush2::calspeed::main_word(rom, vram)); };
        Rebalance r;
        std::fill(std::begin(r.desc), std::end(r.desc), 1.0f);
        std::vector<int> r2_types;
        for (int t = 0; t < ct::rush2_types; t++) {
            if (t != secret_rocket) r2_types.push_back(t);
        }
        double r2_n = (double)r2_types.size();
        auto r2_desc = [&](int t) {
            return (uint32_t)MEM_W(0, (int32_t)(r2_desc_ptrs + MEM_BU(0, (int32_t)(r2_desc_index + t)) * 4));
        };
        auto ratio = [](double r2_mean, double cs_mean) { return cs_mean != 0.0 ? float(r2_mean / cs_mean) : 1.0f; };
        for (auto [from, to] : scaled_fields) {
            for (uint32_t o = from; o < to; o += 4) {
                double a = 0.0, b = 0.0;
                for (int t : r2_types) a += at(r2_desc(t) + o);
                for (int n = 0; n < car_count; n++) b += at(descriptor_of(n) + o);
                r.desc[o / 4] = ratio(a / r2_n, b / car_count);
            }
        }
        // Tire curves: California Speed's two become Rush 2's (the Pickup's +0x68 front, +0x70 rear; every Rush 2 car
        // has the same two), and each car's grip factor stays on top (1.0 on 11 cars, the Dozer 3.0: a mean would leave
        // the ordinary cars below Rush 2's grip).
        uint32_t pickup = r2_desc(0);
        for (int c = 0; c < 2; c++) {
            uint32_t r2_curve = (uint32_t)MEM_W(0, (int32_t)(pickup + (c == 0 ? 0x68 : 0x70)));
            for (int i = 0; i < 6; i++) {
                r.curve[c][i] = ratio(at(r2_curve + i * 4), w((c == 0 ? cs_front_curve : cs_rear_curve) + i * 4));
            }
            // Rollover: a car whose grip is high for how wide its wheels are rolls over before it slides. Every
            // Rush 2 car (but the ROCKET and the DEW) keeps grip / half track at the Pickup's (2.6 / 2.75 front,
            // 3.2 x its rear lateral grip / 2.75 rear); California Speed's narrower cars (the Golf Cart's wheels
            // 0.83 ft from the middle) and its grip factors (the Dozer 3.0) went to 1.06-3.86 on Rush 2's curves
            // and flipped at the slightest turn.
            float grip = at(r2_curve + 16) * (c == 1 ? at(pickup + 0x78) : 1.0f);
            float half_track = std::fabs(at(pickup + (c == 0 ? 0x7C : 0x94)));
            r.roll[c] = half_track > 0.0f ? grip / half_track : 0.0f;
        }
        auto table = [&](uint32_t r2_table, uint32_t cs_table, float cs_scale) {
            double a = 0.0, b = 0.0;
            for (int t : r2_types) a += at(r2_table + t * 4);
            for (int n = 0; n < car_count; n++) b += w(cs_table + n * 4) * cs_scale;
            return ratio(a / r2_n, b / car_count);
        };
        r.steer = table(r2_steer, cs_steer, 1.0f);
        for (int t : r2_types) r.steer_means[0] += at(r2_steer + t * 4) / float(r2_n);
        for (int n = 0; n < car_count; n++) r.steer_means[1] += w(cs_steer + n * 4) / float(car_count);
        r.yaw = table(r2_yaw_damping, cs_yaw_damping, 1.0f);
        r.preload = table(r2_preload, cs_preload, 1.0f);
        r.mass = table(r2_mass, cs_mass, w(cs_mass_scale));
        r.inertia = table(r2_inertia, cs_inertia, 1.0f);
        double handling = 0.0;
        for (int t : r2_types) handling += MEM_BU(0, (int32_t)(r2_handling + t));
        r.handling = (int)std::lround(handling / r2_n);
        return r;
    }

    // Car n's physics: its descriptor (from build_descriptor) mapped by `r`, per-type table entries (every row) and
    // box.
    void build_physics(uint8_t* rdram, const std::vector<uint8_t>& rom, int n, const Rebalance& r) {
        int type = ct::first_calspeed + n;
        int desc = 11 + ct::count_2049 + n; // after Rush 2's 11 and the 2049 cars' descriptors
        uint32_t out = descriptor_of(n);
        auto scale = [&](uint32_t a, float k) {
            MEM_W(0, (int32_t)a) = (int32_t)fbits(f32_of((uint32_t)MEM_W(0, (int32_t)a)) * k);
        };
        for (auto [from, to] : scaled_fields) {
            for (uint32_t o = from; o < to; o += 4) scale(out + o, r.desc[o / 4]);
        }
        for (int c = 0; c < 2; c++) {
            uint32_t curve = ct::calspeed_tire_curves + n * 0x30 + c * 0x18;
            for (int i = 0; i < 6; i++) scale(curve + i * 4, r.curve[c][i]);
            // At most Rush 2's grip per foot of half track (Rebalance::roll), with the car's own rear lateral grip
            // (descriptor +0x78) counted on the rear.
            float half_track = std::fabs(f32_of((uint32_t)MEM_W(0, (int32_t)(out + (c == 0 ? 0x7C : 0x94)))));
            float lateral = c == 1 ? f32_of((uint32_t)MEM_W(0, (int32_t)(out + 0x78))) : 1.0f;
            float grip = f32_of((uint32_t)MEM_W(0, (int32_t)(curve + 16)));
            if (r.roll[c] > 0.0f && lateral > 0.0f) {
                MEM_W(0, (int32_t)(curve + 16)) = (int32_t)fbits(std::min(grip, r.roll[c] * half_track / lateral));
            }
        }
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
        auto scaled = [&](uint32_t table, float k) { return fbits(f32_of(w(table + 4 * n)) * k); };
        put_one(ct::steer_base, scaled(cs_steer, r.steer));
        put_one(ct::yaw_damping, scaled(cs_yaw_damping, r.yaw));
        put_one(ct::preload, scaled(cs_preload, r.preload));
        put_one(ct::mass, scaled(cs_mass, f32_of(w(cs_mass_scale)) * r.mass));
        put_one(ct::inertia, scaled(cs_inertia, r.inertia));
        put_one(ct::yaw_inertia, fbits(crash_threshold));
        rush2::calspeed::CarDefaults def = rush2::calspeed::car_defaults(n);
        put_word(ct::weight, fbits(0.5f));
        put_word(ct::front_wheel_scale, fbits(def.wheel_scale[0]));
        put_word(ct::rear_wheel_scale, fbits(def.wheel_scale[1]));
        put_byte(ct::engine, engines[n]);
        put_byte(ct::torque, 2);                                       // HIGH, as California Speed's map index
        put_byte(ct::drive, main_byte(rom, cs_drive + n));
        put_byte(ct::suspension, suspension);
        put_byte(ct::tires, r.handling); // California Speed's is 4 for every car
        put_byte(ct::rims, def.rims);
        put_byte(ct::main_color, def.main);
        put_byte(ct::accent_color, def.accent);
        put_byte(ct::stripe, 0);
        put_byte(ct::stripe_color, 0);
        for (int i = 0; i < 4; i++) {
            MEM_W(0, (int32_t)(ct::boxes + type * 16 + i * 4)) = (int32_t)w(cs_boxes + 16 * n + 4 * i);
        }
    }

    // Every car's descriptor (build_descriptor) and the mapping onto Rush 2's roster from them.
    Rebalance map_cars(uint8_t* rdram, const std::vector<uint8_t>& rom) {
        for (int n = 0; n < car_count; n++) build_descriptor(rdram, rom, n, descriptor_of(n));
        return rebalance(rdram, rom);
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
        Rebalance r = map_cars(rdram, *rom);
        mapping = r;
        int served = 0;
        for (int n = 0; n < car_count; n++) {
            std::vector<uint8_t> car;
            std::string error;
            if (!load_car(*rom, n, car, error)) {
                fprintf(stderr, "[CalSpeed] Couldn't convert car %d (%s): %s\n", n + 1, source_prefixes[n], error.c_str());
                continue;
            }
            build_physics(rdram, *rom, n, r);
            rush2::car2049::set_calspeed_car(rdram, n, std::move(car), part_prefix(n), palette);
            served++;
        }
        rush2::car2049::set_calspeed_logos(rdram, rush2::calspeed::name_logos(*rom));
        cars_loaded = served == car_count;
        printf("[CalSpeed] %d cars\n", served);
    }
}

bool rush2::calspeed::native_car(uint8_t* rdram, int n, uint32_t desc, uint32_t curves, NativeCar& out) {
    std::shared_ptr<const std::vector<uint8_t>> rom = loaded_rom;
    if (!cars_loaded || rom == nullptr || n < 0 || n >= car_count) return false;
    build_descriptor(rdram, *rom, n, desc, curves, true);
    auto w = [&](uint32_t vram) { return f32_of(rush2::calspeed::main_word(*rom, vram)); };
    uint32_t d = rush2::calspeed::main_word(*rom, cs_desc_ptrs + 4 * main_byte(*rom, cs_desc_index + n));
    out.steer = w(cs_steer + 4 * n);
    out.yaw = w(cs_yaw_damping + 4 * n);
    out.preload = w(cs_preload + 4 * n);
    out.mass = w(cs_mass + 4 * n) * w(cs_mass_scale);
    out.inertia = w(cs_inertia + 4 * n);
    out.tires = 4;
    out.grip_lateral = w(d + 0x6C);
    out.grip_long = w(d + 0x70);
    return true;
}

// The inverse of build_physics' mapping. The steer base alone is moved by the means' difference instead of scaled: Rush
// 2's runs 0-1200 (0 on the SLED, BUGATTI, CONCEPT and GT90, which turn on their tires and yaw gain) against California
// Speed's 10000 on every car, and as the slide term (func_80069E74: what keeps California Speed's cars from spinning)
// a scaled 0 would leave those cars none.
bool rush2::calspeed::native_values(uint8_t* rdram, int type, uint32_t desc, uint32_t curves, NativeCar& out) {
    if (!cars_loaded || ct::is_calspeed(type)) return false;
    const Rebalance& r = mapping;
    auto at = [&](uint32_t a) { return f32_of((uint32_t)MEM_W(0, (int32_t)a)); };
    auto put = [&](uint32_t a, float f) { MEM_W(0, (int32_t)a) = (int32_t)fbits(f); };
    auto undo = [](float v, float k) { return k != 0.0f ? v / k : v; };
    uint32_t from = (uint32_t)MEM_W(0, (int32_t)(ct::desc_ptrs + MEM_BU(0, (int32_t)ct::entry(ct::desc_index, 0, type)) * 4));
    for (uint32_t o = 0; o < ct::desc_size; o += 4) MEM_W(0, (int32_t)(desc + o)) = MEM_W(0, (int32_t)(from + o));
    for (auto [lo, hi] : scaled_fields) {
        for (uint32_t o = lo; o < hi; o += 4) put(desc + o, undo(at(desc + o), r.desc[o / 4]));
    }
    // Tire curves (+0x68 per wheel, the fronts' then the rears'): Rush 2's two become California Speed's.
    for (int i = 0; i < 4; i++) {
        int c = i < 2 ? 0 : 1;
        uint32_t src = (uint32_t)MEM_W(0, (int32_t)(from + 0x68 + 4 * i)), dst = curves + c * 0x18;
        for (int k = 0; k < 6; k++) put(dst + 4 * k, undo(at(src + 4 * k), r.curve[c][k]));
        MEM_W(0, (int32_t)(desc + 0x68 + 4 * i)) = (int32_t)dst;
    }
    out.steer = std::max(0.0f, at(ct::entry(ct::steer_base, 0, type)) + r.steer_means[1] - r.steer_means[0]);
    out.yaw = undo(at(ct::entry(ct::yaw_damping, 0, type)), r.yaw);
    out.preload = undo(at(ct::entry(ct::preload, 0, type)), r.preload);
    out.mass = undo(at(ct::entry(ct::mass, 0, type)), r.mass);
    out.inertia = undo(at(ct::entry(ct::inertia, 0, type)), r.inertia);
    out.tires = 4;
    out.grip_lateral = out.grip_long = 1.0f;
    return true;
}

rush2::calspeed::CarDefaults rush2::calspeed::car_defaults(int n) {
    CarDefaults d;
    if (n < 0 || n >= car_count) return d;
    d.main = nearest_color(default_rgb[n][0]);
    d.accent = nearest_color(default_rgb[n][1]);
    d.rims = default_rims[n];
    d.wheel_scale[0] = default_wheel_scale[0][n];
    d.wheel_scale[1] = default_wheel_scale[1][n];
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

void rush2::calspeed::prepare_cars(const std::vector<uint8_t>& rom) {
    for (int n = 0; n < car_count; n++) {
        std::vector<uint8_t> car;
        std::string error;
        if (load_car(rom, n, car, error)) rush2::track_cache::preload_blob(car_cache_name(n));
    }
    rush2::calspeed::name_logos(rom);
    rush2::track_cache::preload_blob("calspeed_name_logos");
}

bool rush2::calspeed::build_physics_image(uint8_t* rdram, const std::vector<uint8_t>& rom) {
    std::vector<uint8_t> palette;
    if (!cars_option.load(std::memory_order_relaxed) || !load_palette(rom, palette)) return false;
    Rebalance r = map_cars(rdram, rom);
    for (int n = 0; n < car_count; n++) build_physics(rdram, rom, n, r);
    return true;
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
