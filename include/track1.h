#ifndef __TRACK1_H__
#define __TRACK1_H__

#include <array>
#include <cstdint>
#include <map>
#include <functional>
#include <memory>
#include <string>
#include <vector>

// San Francisco Rush (Rush 1) race tracks added to Rush 2 (src/track1*.cpp, src/rush1_rom.cpp,
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

    // The user's Rush 1 ROM (src/rush1_rom.cpp): San Francisco Rush (USA), big-endian, or null.
    std::shared_ptr<const std::vector<uint8_t>> get_rom();
    bool rom_available();
    // The SF Rush settings, shown in the Games tab (src/games_tab.cpp): adding them (before
    // recompui::config::finalize()), the tab's section, saving, and loading them and the stored ROM (after finalize).
    void init_config();
    void add_games_section(rush2::ui::OptionsPage* page, std::function<void()>& refresh);
    void save_config();
    void load_config();

    // The SF Rush Tracks option. Tracks are offered when it is on and the ROM is present.
    void set_option(bool enabled);
    bool available();

    // Rush 1's files: asset index 0-71 of its asset table (all LZ compressed), and its main code segment.
    bool read_asset(const std::vector<uint8_t>& rom, int index, std::vector<uint8_t>& out);
    // Main code (decompressed, loaded at main_vram), cached per ROM. Null if the ROM can't be read.
    constexpr uint32_t main_vram = 0x8005BB10;
    std::shared_ptr<const std::vector<uint8_t>> main_code(const std::vector<uint8_t>& rom);

    // A Rush 1 track converted to the files of a Rush 2 track slot (src/track1_convert.cpp, port of
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
        };
        Timing timing[2];
        // Breakables (src/track1.cpp redirects Rush 2's model lookups during the race): placement record name -> the
        // model it draws instead of its class's Rush 2 model, and Rush 2 breakable piece name -> Rush 1 piece model.
        std::map<std::string, std::string> record_models;
        std::map<std::string, std::string> piece_models;
    };
    // t: Rush 1 track 0-6. prefix: the host slot's track prefix (the placement tree and <prefix>FINISH model names).
    bool convert_track(const std::vector<uint8_t>& rom, int t, const std::string& prefix, ConvertedTrack& out,
                       std::string& error);

    // Track select art: appends a diorama model R1TRACKn (Rush 1's own track select model) and a 128x32 name logo
    // R1LOGOn per Rush 1 track to a Rush 2 model container (asset 3, or Rush 2049's copy of it).
    bool extend_menu_container(const std::vector<uint8_t>& container, const std::vector<uint8_t>& rom,
                               std::vector<uint8_t>& out);
    // Scale for the track select's diorama table that makes track t's diorama the size of the stock ones.
    float diorama_scale(const std::vector<uint8_t>& rom, int t);
    // Rush 2's in-race logo container for the host slot (asset 4 + host) with its texture replaced by track t's logo.
    bool build_race_logo(const std::vector<uint8_t>& rush2_logo, const std::vector<uint8_t>& rom, int t,
                         std::vector<uint8_t>& out);

    // The Rush 1 track (1-7) being raced, or 0 (src/track1.cpp).
    int race_track();
    void set_race_track(int k);
    // Puts the host slot's own files and table entries back (call from a game thread).
    void restore_host(uint8_t* rdram);
    // Race file loading (start of func_800A4C98): serves the raced Rush 1 track's files and returns true, or puts the
    // host's own back and returns false when no Rush 1 track is raced.
    bool load(uint8_t* rdram);
    // func_8007C27C at 0x8007C480: the section mask for the camera's region. Returns true if it handled it.
    bool pvs(uint8_t* rdram, uint32_t sp);
    // Seed time in seconds for the record tables of track k (1-7), or 0 if unknown.
    float record_seed(int k, bool backward);
    // func_80093048 at 0x80093298: replaces the race time Rush 2 worked out from the AI lanes with Rush 1's own.
    // Returns true if a Rush 1 track is raced.
    bool race_time(uint8_t* rdram);
}

#endif
