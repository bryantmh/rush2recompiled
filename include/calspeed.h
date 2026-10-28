#ifndef __CALSPEED_H__
#define __CALSPEED_H__

#include <array>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "car2049.h"
#include "track2049.h"

// California Speed race tracks added to Rush 2 (src/calspeed/, docs/calspeed_research.md).
//
// California Speed's N64 version is built on the Rush 2 engine: its tracks are Rush 2 files (model containers,
// placement, collision and AI paths, all LZ compressed as Rush 2's) and its per-track tables have Rush 2's layout. The
// track select offers its 16 tracks as ids 64-79, after the custom tracks. A race runs in the host slot the Rush 2049
// and SF Rush tracks use (rush2::track2049::host_slot): src/rush2049/track2049.cpp swaps the slot's files and table
// entries for the Cal Speed track's while it is raced (k = calspeed_k + track).
//
// Eleven of the tracks run from a start to a finish line (checkpoint 0 is the start, the flag-1 checkpoint the finish,
// as Rush 2049's obstacle course); they are raced over one lap. Clover, Fuji, Oval, California and San Andreas are
// circuits (checkpoint 0 is start, finish and lap line) and take the LAPS option. California Speed has no backward
// races, so none of its tracks is raced backward.
namespace rush2::ui {
    class OptionsPage;
}

namespace rush2::calspeed {
    constexpr int track_count = 16;
    constexpr int first_menu_id = 64;   // Track select id of track 0 (after the custom tracks, 38-63).
    constexpr int calspeed_k = 2000;    // rush2::track2049's track number of track 0 (k = calspeed_k + t).
    // California Speed's own names for its tracks (0x8008A588, 20 bytes each; "Cal" is shown as California).
    constexpr const char* track_names[track_count] = { "Monterey", "Silicon Valley", "Highway 1", "San Francisco",
        "Central Valley", "Santa Cruz", "San Diego", "Clover", "Los Angeles", "Mt. Shasta", "Yosemite", "Mojave Desert",
        "Fuji", "Oval", "California", "San Andreas" };
    // Track t runs from a start to a finish line (its path's checkpoint 0 has flag 2 only); the others are circuits
    // (checkpoint 0 has flags 7). Read from the 16 path files (tools/calspeed/cs.py).
    constexpr bool point_to_point(int t) {
        return t >= 0 && t < track_count && t != 7 && t != 12 && t != 13 && t != 14 && t != 15;
    }
    inline bool is_menu_track(int id) {
        return id >= first_menu_id && id < first_menu_id + track_count;
    }

    // The user's California Speed (USA) ROM, big-endian, or null (src/calspeed/calspeed_rom.cpp).
    std::shared_ptr<const std::vector<uint8_t>> get_rom();
    bool rom_available();
    // The California Speed settings, shown in the Games tab (src/games_tab.cpp) and kept in its config (games.json):
    // adding them (before recompui::config::finalize()), the tab's section, and loading the stored ROM (after
    // rush2::wings::load_config()).
    void init_config();
    void add_games_section(rush2::ui::OptionsPage* page, std::function<void()>& refresh);
    void load_config();

    // The California Speed Tracks option. Tracks are offered when it is on and the ROM is present.
    void set_option(bool enabled);
    bool available();
    // The California Speed Extended Scenery option: load_track adds the scenery of src/calspeed/calspeed_scenery.cpp
    // (include/calspeed_scenery.h). Read when a track is converted, so a change takes effect from the next race.
    bool scenery_enabled();
    // The California Speed Billboard Trees option: load_track turns the tracks' flat single trees to face the camera
    // (src/calspeed/calspeed_billboards.cpp). Read when a track is converted, so a change takes effect from the next race.
    bool billboards_enabled();

    // Asset `index` (0-127) of California Speed's asset table (ROM 0x5760C), decompressed.
    bool read_asset(const std::vector<uint8_t>& rom, int index, std::vector<uint8_t>& out);
    // A word of its main code (raw in the ROM at 0x1CD60, run at 0x80053710), or 0 outside it.
    uint32_t main_word(const std::vector<uint8_t>& rom, uint32_t vram);
    // The ROM's hash, the source key of its cached conversions (src/track_cache.cpp).
    uint64_t rom_source(const std::vector<uint8_t>& rom);

    // A Cal Speed track converted for a Rush 2 slot (src/calspeed/calspeed_convert.cpp).
    struct Converted {
        rush2::track2049::ConvertedTrack track;   // geometry, placement, collision, paths, visibility, fog
        uint32_t cloud_scroll = 0, sky_parallax = 0;   // f32 bits, from Cal Speed's tables (0x80094C2C, 0x80094C6C)
        // Breakables, as for the SF Rush tracks (src/rush1/track1.cpp redirects Rush 2's lookups while the track is
        // raced): placement record -> the model it draws, and Rush 2 breakable piece -> Cal Speed model.
        std::map<std::string, std::string> record_models;
        std::map<std::string, std::string> piece_models;
    };
    // t: track 0-15. prefix: the host slot's track prefix. shared_models: Rush 2's shared model names (assets 0x12,
    // 0x14), which win over the track's in Rush 2's lookups. scenery: add the extended scenery. billboards: turn its
    // single trees to the camera. The track comes from the disk cache when this build converted it from the same ROM
    // (and settings) before.
    bool load_track(const std::vector<uint8_t>& rom, int t, const std::string& prefix,
                    const std::set<std::string>& shared_models, bool scenery, bool billboards, Converted& out,
                    std::string& error);
    // Converts track t into the disk cache as the scenery options are set now, unless it is there (src/prewarm.cpp).
    void prepare_track(const std::vector<uint8_t>& rom, int t, const std::string& prefix,
                       const std::set<std::string>& shared_models);
    // Reads track t's cache entry (as the options are set now) into memory, for the race about to load it.
    void preload_track(int t);

    // Track select art: per track a diorama (California Speed's own track select landmark, asset 6) and a 128 x 32
    // name logo made from its track select banner (asset 3), named menu_model_name / menu_logo_name. Also gives the
    // renderer the logos' exact colors in place of their CI8 copies (rush2::upscale::add_exact_image).
    std::string menu_model_name(int t);
    std::string menu_logo_name(int t);
    bool menu_art(const std::vector<uint8_t>& rom, std::vector<rush2::track2049::MenuModel>& models,
                  std::vector<rush2::track2049::MenuLogo>& logos);
    // The track select's diorama scale for track t (its landmark the size of Rush 2's dioramas).
    float diorama_scale(const std::vector<uint8_t>& rom, int t);
    // Rush 2's in-race logo container for the host slot (asset 4 + host) with its texture replaced by track t's logo.
    bool build_race_logo(const std::vector<uint8_t>& rush2_logo, const std::vector<uint8_t>& rom, int t,
                         std::vector<uint8_t>& out);
    // Seed time in seconds for the record tables of track t: the time its AI lane 0 takes over one lap, or 0.
    float record_seed(int t);
    // An AI lane's target speeds (true mph, as California Speed's drone code reads them) capped where a Rush 2 car
    // can't hold them: the lane's curve limits the speed and the braking before it (docs/calspeed_research.md#5-paths).
    // pos: each point's x, y, z; loop: a circuit's lane (its last point leads to its first); corner_g, crest_g: the
    // most a curve, a crest or a dip may take (crest_g 0: crests and dips not capped).
    std::vector<uint8_t> capped_lane_speeds(const std::vector<std::array<float, 3>>& pos,
                                            const std::vector<uint8_t>& mph, bool loop, double corner_g = 2.5,
                                            double crest_g = 4.0);

    // The Cal Speed track (1-16) raced in the host slot, or 0.
    int race_track();
    // A California Speed track is being raced now (not the attract demo): its physics hooks run.
    bool racing(uint8_t* rdram);
    void set_race_track(int k);
    // The raced track's converted path file, whose racing line tells when a car has fallen under the course, and its
    // collision, whose ground tells where a reset has road under it.
    void set_race_path(const std::vector<uint8_t>& path, const std::vector<uint8_t>& collision);

    // A ground polygon of a converted track's collision as California Speed's wheel query reads it: its first vertex
    // in the world, the rows mapping world to its frame, and every vertex's x and z in that frame.
    struct GroundPolygon {
        float origin[3];
        float m[9];
        std::vector<std::array<float, 2>> xz;
    };
    std::vector<GroundPolygon> ground_polygons(const std::vector<uint8_t>& collision);
    // California Speed's func_800AE884 as func_800AF1EC calls it: whether p is over the polygon by more than -5 ft and
    // less than `best`, and if so how far (along the polygon's normal).
    bool ground_under(const GroundPolygon& g, const float p[3], float best, float& height);

    // California Speed's 21 cars as car types 36-56 (src/calspeed/calspeed_cars.cpp, docs/calspeed_research.md#cars),
    // in its order, with its names (0x8008A6D4, 20 bytes each).
    constexpr int car_count = 21;
    constexpr const char* car_names[car_count] = { "SPORTSTER", "MUSCLE", "486SE", "PREDATOR", "MANO", "FIVE OH",
        "MT DEW", "HOPPER", "FAIRCHILD", "INSECT", "MERCADO", "GOLF CART", "SQUIRREL", "FORKLIFT", "CONV", "SLED", "BAJA",
        "OL TRUCK", "CAMPER", "SEMI", "DOZER" };
    // The California Speed Cars option (Games tab): whether the car select offers them. They are offered while it is
    // on and the ROM's cars are loaded.
    void set_cars_option(bool enabled);
    // Its cars' car select name logos in its own lettering, cut from its car select bank (src/calspeed/calspeed_logos.cpp);
    // text-only entries if the bank can't be read.
    std::vector<rush2::car2049::NameLogo> name_logos(const std::vector<uint8_t>& rom);
    bool cars_available();
    // The California Speed Computer Cars option: where the computer cars may drive them (Rush2049Tracks means
    // California Speed's tracks here).
    void set_drone_cars(rush2::car2049::DroneCars mode);
    rush2::car2049::DroneCars drone_cars();
    // Car n's default colors (as Rush 2's paint colors 0-31), TIRE RIMS and front and rear wheel model scale (Rush 2's
    // TIRE SIZE F / R): California Speed's own.
    struct CarDefaults {
        int main = 0, accent = 0, rims = 0;
        float wheel_scale[2] = { 1.0f, 1.0f };
    };
    CarDefaults car_defaults(int n);
    // Converts and serves the cars (assets, physics, logos) from the ROM, if it is in: at boot after the type tables are
    // built (rush2::car2049::init_physics), and each frame (check_cars) once a ROM is chosen later, in the menus.
    void init_cars(uint8_t* rdram);
    void check_cars(uint8_t* rdram);
    // Converts the cars and name logos into the disk cache (those not there yet) and reads them into memory, so
    // init_cars only installs them. Any thread (src/prewarm.cpp, at launch).
    void prepare_cars(const std::vector<uint8_t>& rom);
    // The cars' physics alone (descriptors, type table entries, boxes) as init_cars builds them, with no assets or
    // logos, into a memory image of its own before the game starts (rush2::car2049::prepare_bar_ranges, after
    // rush2::car2049::init_physics on it). Whether the cars will be available (cars_available) once the boot loads them.
    bool build_physics_image(uint8_t* rdram, const std::vector<uint8_t>& rom);
    // California Speed car n's own physics values, unscaled, for a computer car running California Speed's physics
    // (src/calspeed/calspeed_physics.cpp): its descriptor built into `desc` (Rush 2's layout) with its two tire curves
    // as California Speed has them at `curves` (12 floats), its per-type table values, and its descriptor's two grip
    // factors (+0x6C across, +0x70 along), which California Speed applies to every wheel's tire forces. False while
    // the cars aren't loaded.
    struct NativeCar {
        float steer = 0, yaw = 0, preload = 0, mass = 0, inertia = 0;
        int tires = 4;
        float grip_lateral = 1.0f, grip_long = 1.0f;
    };
    bool native_car(uint8_t* rdram, int n, uint32_t desc, uint32_t curves, NativeCar& out);
    // The same for a car of any other game (Rush 2, 2049, a mod; car `type`): its own values in California Speed's
    // terms, the California Speed cars' mapping onto Rush 2's roster (Rebalance) undone, so it drives in California
    // Speed's physics as a California Speed car would. Its descriptor copied to `desc` with the mapped fields and its
    // tire curves (at `curves`) undone; grip factors 1 (its own rear grip, +0x78, stays in the descriptor).
    bool native_values(uint8_t* rdram, int type, uint32_t desc, uint32_t curves, NativeCar& out);

    // The CS Physics option (Games tab, docs/calspeed_research.md#cs-physics-california-speeds-world): on California
    // Speed's tracks, Rush 2's world (the CS Road Hold port, the default) or California Speed's own physics. A car's
    // role is set when the race sets it up, so a change takes effect from the next race.
    namespace physics {
        enum class Mode { Rush2World, CalSpeed, Rushified };
        void set_mode(Mode m);
        Mode get_mode();
        bool race_mode();   // the option runs California Speed's physics, Full CS or Partial (whatever the track)
        // California Speed's physics run for this car now (a California Speed track is raced and the car was set up
        // under the option); native: on California Speed's values (a California Speed car's own, any other car's
        // mapped back from Rush 2's roster); converted: the fallback when those can't be had, its own values converted
        // for the world.
        bool active(uint8_t* rdram, uint32_t car);
        bool native(uint8_t* rdram, uint32_t car);
        bool converted(uint8_t* rdram, uint32_t car);
        // A converted car's descriptor copy, for California Speed's heavier road hold.
        void convert_descriptor(uint8_t* rdram, uint32_t desc);
        // Called from the ground contact (calspeed_walls.cpp): its start, a wall hit on a body point, its end.
        void ground_contact_begin(uint8_t* rdram, uint32_t car);
        void wall_hit(uint32_t car, int point, int type);
        void ground_contact_end(uint8_t* rdram, uint32_t car);
        // Called from the hooks of calspeed_rom.cpp at the same spots; true when they ran (the callers' own rules
        // then don't).
        void wheel_forces_end(uint8_t* rdram, uint32_t car);
        bool tire_load(uint8_t* rdram, recomp_context* ctx);
        bool tire_grip(uint8_t* rdram, recomp_context* ctx);
        bool force_sum(uint8_t* rdram, uint32_t car);
        void torque_end(uint8_t* rdram, uint32_t car);
        void orient_begin(uint8_t* rdram, uint32_t car);
        void orient_end(uint8_t* rdram, uint32_t car);
    }
}

#endif
