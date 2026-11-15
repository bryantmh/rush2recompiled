#ifndef __TRACK1_H__
#define __TRACK1_H__

#include <array>
#include <cstdint>
#include <map>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "diorama_look.h"

// San Francisco Rush (Rush 1) race tracks added to Rush 2 (src/rush1/track1*.cpp, src/rush1/rush1_rom.cpp,
// docs/rush1_research.md).
//
// The track select offers ids 18-24 after Rush 2's 12 tracks and Rush 2049's 6. A race runs in the same borrowed
// slot as the Rush 2049 tracks (rush2::track2049::host_slot); its files and per-track table entries are swapped for
// the Rush 1 track's while it is raced.
namespace rush2::ui {
    class OptionsPage;
}

namespace rush2::track1 {
    constexpr int track_count = 7;
    constexpr int first_menu_id = 18;   // Track select id of Rush 1 track 1.
    // Rush 1 track 0-6's name, from the Rush the Rock arcade update (the N64 game calls them Track 1-7).
    constexpr const char* track_names[track_count] = { "Golden Gate", "Embarcadero", "Market", "Downtown",
                                                       "The Heights", "Sunset", "The Rock" };

    // The user's Rush 1 ROM (src/rush1/rush1_rom.cpp): San Francisco Rush (USA), big-endian, or null.
    std::shared_ptr<const std::vector<uint8_t>> get_rom();
    // Rush 2's ROM image (recomp::get_rom) is in its final buffer: the game's boot grew it for the SF Rush and
    // California Speed music (src/rush1/track1_audio.cpp). Threads off the game thread wait for this before reading it.
    bool rom_image_final();
    bool rom_available();
    // The SF Rush settings, shown in the Games tab (src/games_tab.cpp) and kept in its config with Rush 2049's
    // (src/rush2049/wings.cpp): adding them (before recompui::config::finalize()), the tab's section, and loading the stored
    // ROM (after rush2::wings::load_config()).
    void init_config();
    void add_games_section(rush2::ui::OptionsPage* page, std::function<void()>& refresh);
    void load_config();

    // The SF Rush Tracks option. Tracks are offered when it is on and the ROM is present.
    void set_option(bool enabled);
    bool available();

    // Rush 1's files: asset index 0-71 of its asset table (all LZ compressed), and its main code segment.
    bool read_asset(const std::vector<uint8_t>& rom, int index, std::vector<uint8_t>& out);
    // Main code (decompressed, loaded at main_vram), cached per ROM. Null if the ROM can't be read.
    constexpr uint32_t main_vram = 0x8005BB10;
    std::shared_ptr<const std::vector<uint8_t>> main_code(const std::vector<uint8_t>& rom);

    // Rush 1 model container `index` (7-35: menus, shared race objects and cars, docs/rush1_research.md section 1.1) as
    // a Rush 2 container with every model under its own name, for the model dump (src/asset_catalog.cpp). Its lists are
    // translated and flattened as convert_track does. Its texture table is left out (a record doesn't say whether its
    // data is texels or a load list); the models' lists load their textures themselves.
    bool convert_container(const std::vector<uint8_t>& rom, int index, std::vector<uint8_t>& out, std::string& error);
    // The container of the race objects every track uses (cones, meters, trees, the key KEYL1).
    constexpr int shared_objects_asset = 12;

    // A Rush 1 track converted to the files of a Rush 2 track slot (src/rush1/track1_convert.cpp, port of
    // tools/rush1/track1.py).
    struct ConvertedTrack {
        std::vector<uint8_t> geometry;      // Asset 0x33 + slot.
        std::vector<uint8_t> placement;     // Asset 0x3F + slot.
        std::vector<uint8_t> collision[2];  // Asset 0x4B + slot, forward and backward (Rush 1 has one per direction).
        std::vector<uint8_t> path[2];       // Assets 0x57 + slot and 0x63 + slot.
        std::vector<uint8_t> pvs;           // 16 bytes per camera region.
        uint8_t pvs_count = 0;
        std::vector<int16_t> demo_starts[2];
        float lap_seconds[2] = {};          // Lap time of the AI lanes, for the record table seeds.
        // Rush 1's race timer values per direction (its checkpoint records, docs/rush1_research.md): the start
        // time's base and each checkpoint's extension by lap (1, 2, 3+), in seconds before the difficulty factor.
        struct Timing {
            int16_t start = 0;
            std::vector<std::array<int16_t, 3>> checkpoints;
            // Checkpoints with Rush 1's flag 4: no wrong-way warning while one is the car's last or next checkpoint.
            uint16_t no_wrong_way = 0;
            // Each checkpoint's own gate radius squared (record +0xC), which Rush 1's gate test uses (func_800A0BB8).
            std::vector<uint32_t> radius2;
        };
        Timing timing[2];
        // Breakables (src/rush1/track1.cpp redirects Rush 2's model lookups during the race): placement record name -> the
        // model it draws instead of its class's Rush 2 model, and Rush 2 breakable piece name -> Rush 1 piece model.
        std::map<std::string, std::string> record_models;
        std::map<std::string, std::string> piece_models;
    };
    // t: Rush 1 track 0-6. prefix: the host slot's track prefix (the placement tree and <prefix>FINISH model names).
    // look: Look::night converts the course as a NIGHT version (docs/sfrush_ps1.md#n64-night-versions): its TLUTs and
    // untextured shades in the PlayStation SF Rush's night colors, and a night sky model SKYO1.
    bool convert_track(const std::vector<uint8_t>& rom, int t, const std::string& prefix, ConvertedTrack& out,
                       std::string& error, rush2::diorama_look::Look look = rush2::diorama_look::Look::day);
    // The PlayStation's night art for the NIGHT versions' buildings (docs/sfrush_ps1.md#n64-night-art): the PS1 mod's
    // n64_night_art.bin (src/sfrush_ps1/ps1_track.cpp n64_night_art), N64 texture key -> the texture in the PS1's night
    // texels. A night conversion puts it in place of each texture it has, and lights no windows of its own guessing;
    // with none set (no PS1 mod) it lights them by guess as before. Any thread; part of the night tracks' cache key
    // (night_art_hash).
    void set_night_art(std::shared_ptr<const std::vector<uint8_t>> art);
    // Sunset's traffic bus `bus` (0-7, src/rush1/track1_buses.cpp) while the buses run: its position (world units)
    // and axes (rows: right, up, forward; the battle_render::place form). False when the buses aren't running.
    bool bus_pose(int bus, float pos[3], float m[9]);
    std::shared_ptr<const std::vector<uint8_t>> night_art();
    uint64_t night_art_hash();
    // The same for the SNOW versions (docs/sfrush_ps1.md#n64-snow-versions): the PS1 mod's n64_snow_art.bin, the
    // PS1's snow texels of the mapped textures in n64_night_art.bin's form. A snow conversion puts them in place; the
    // other textures keep their day colors.
    void set_snow_art(std::shared_ptr<const std::vector<uint8_t>> art);
    std::shared_ptr<const std::vector<uint8_t>> snow_art();
    uint64_t snow_art_hash();

    // Track select art: appends a diorama model R1TRACKn (Rush 1's own track select model) and a 128x32 name logo
    // Gives the renderer the track banners' exact colors in place of their CI8 copies' 255 (src/texture_upscale.cpp,
    // add_exact_image). Called once at startup.
    void add_banner_images();
    // Track t's (0-6) banner as a custom track's logo is kept (rush2::track_mod::MenuArt logo, logo_palette, logo_rgba):
    // a version raced on the course ([track] base) shows its course's.
    void course_logo(int t, std::vector<uint8_t>& texels, std::vector<uint8_t>& palette, std::vector<uint8_t>& rgba);
    // R1LOGOn per Rush 1 track to a Rush 2 model container (asset 3, or Rush 2049's copy of it).
    bool extend_menu_container(const std::vector<uint8_t>& container, const std::vector<uint8_t>& rom,
                               std::vector<uint8_t>& out);
    // Scale for the track select's diorama table that makes track t's diorama the size of the stock ones.
    float diorama_scale(const std::vector<uint8_t>& rom, int t);
    // Rush 2's in-race logo container for the host slot (asset 4 + host) with its texture replaced by track t's logo.
    bool build_race_logo(const std::vector<uint8_t>& rush2_logo, const std::vector<uint8_t>& rom, int t,
                         std::vector<uint8_t>& out);

    // Breakable redirects as an SF Rush track's (record -> model it draws, Rush 2 piece -> model) for another game's
    // track in the host slot (California Speed, src/rush2049/track2049.cpp), with Rush 2's own hit test, or SF Rush's
    // with rush1_hits (an arcade version of an SF Rush course: its keys, src/arcade2049/arcade_collectibles.cpp);
    // cleared again with clear_redirects, which leaves an applied SF Rush track's alone.
    void set_redirects(uint8_t* rdram, const std::map<std::string, std::string>& records,
                       const std::map<std::string, std::string>& pieces, bool rush1_hits = false);
    void clear_redirects();

    // Converts every track into the disk cache (those not there yet) and keeps their lap times for record_seed. Any
    // thread (src/prewarm.cpp, at launch).
    void prepare(const std::vector<uint8_t>& rom);
    // Reads track k's (1-7) cache entry into memory now, for the race about to load it (src/prewarm.cpp, from the car
    // select). Any thread.
    void preload(int k, rush2::diorama_look::Look look = rush2::diorama_look::Look::day);

    // The Rush 1 track (1-7) being raced, or 0 (src/rush1/track1.cpp).
    int race_track();
    // Sets the raced track, as its N64 (day) version.
    void set_race_track(int k);
    // A version of an N64 course raced on the course itself in another look (a custom track whose track.toml has
    // [track] base = "sfrush:<course>", docs/custom_tracks.md#for-developers; the PlayStation SF Rush mod's N64 NIGHT
    // versions, docs/sfrush_ps1.md#n64-night-versions): the course's own files converted in `look`, with the race's
    // fog color and the cars' light (0xRRGGBBFF, 0 none) from that track.toml. Call after set_race_track.
    struct RaceLook {
        rush2::diorama_look::Look look = rush2::diorama_look::Look::day;
        std::array<uint8_t, 3> fog = { 0x96, 0x96, 0xBE };
        uint32_t car_light = 0;
        float snowfall = 0.0f;   // [look] snowfall (src/snowfall.cpp), 0 none
    };
    void set_race_look(const RaceLook& look);
    // The applied track's look (day when none is applied).
    rush2::diorama_look::Look applied_look(uint8_t* rdram);
    // The applied track's car light (0xRRGGBBFF), 0 for none or when no SF Rush track is applied.
    uint32_t car_light(uint8_t* rdram);
    // The applied track's falling snow ([look] snowfall of the version raced on it), 0 for none.
    float snowfall(uint8_t* rdram);
    // The applied track has its own sky model (a night version's SKYO1).
    bool sky_model(uint8_t* rdram);
    // Converts course k (1-7) in `look` into the disk cache unless it is there (a version's, ahead of its race). Any
    // thread.
    void prepare_look(const std::vector<uint8_t>& rom, int k, rush2::diorama_look::Look look);
    // Puts the host slot's own files and table entries back (call from a game thread).
    void restore_host(uint8_t* rdram);
    // Race file loading (start of func_800A4C98): serves the raced Rush 1 track's files and returns true, or puts the
    // host's own back and returns false when no Rush 1 track is raced.
    bool load(uint8_t* rdram);
    // func_8007C27C at 0x8007C480: the section mask for the camera's region. Returns true if it handled it, with
    // table = its region mask table.
    bool pvs(uint8_t* rdram, uint32_t sp, uint32_t& table);
    // The camera position (vec3 f32 in RDRAM) of the view func_8007C27C is working on, 0 before the first.
    uint32_t pvs_camera_position();
    // Seed time in seconds for the record tables of track k (1-7): Rush 1's own (0x800D157C), else lane 1's lap time,
    // or 0 if unknown.
    float record_seed(int k, bool backward);
    // func_80093048 at 0x80093298: replaces the race time Rush 2 worked out from the AI lanes with Rush 1's own.
    // Returns true if a Rush 1 track is raced.
    bool race_time(uint8_t* rdram);
}

#endif
