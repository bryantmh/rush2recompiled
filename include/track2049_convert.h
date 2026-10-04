#ifndef __TRACK2049_CONVERT_H__
#define __TRACK2049_CONVERT_H__

#include <cstdint>
#include <set>
#include <string>
#include <vector>

// Conversion of a Rush 2049 race track to the files of a Rush 2 track slot (src/track2049_convert.cpp). Port of
// tools/rush2049/track.py; its output is byte-identical (tools/rush2049/cpp_test checks this).
namespace rush2::track2049 {
    // A placement record holding a Rush 2049 path object at one of its spawn nodes (static_paths).
    struct PathRecord {
        int path;   // PTHD index in the 2049 geometry file
        int node;   // spawn node index in that path
        int record; // record index in the placement file
    };

    // A placement record of a Rush 2049 object that turns in place (TROLLEY2, WINDMILL, WINDMILL2).
    struct SpinRecord {
        int record; // record index in the placement file
        int sub;    // 2049 type sub-kind (3-5), which selects the rate
    };

    // Rush 2049's animated track textures (docs/rush2049_research/texanim.md), as patch sites in the converted
    // geometry. Offsets and addresses are geometry-relative; Rush 2 loads the file at some base and rebases each
    // G_SETTIMG w1 of the texture-load lists to ((w1 + base) & 0xFFFFFF) | (w1 & 0x0F000000).

    // A flip-book (2049 main 0x8011A31C, run by func_800BD2C8 / set up by func_800BDAA8): every `period` seconds the
    // step moves on one frame and the target texture's load list loads that frame's texels instead.
    struct TexFlipbook {
        std::string target;            // Target texture name (= frame `start`).
        uint32_t settimg = 0;          // Offset of the target load list's first G_SETTIMG, whose w1 is replaced.
        std::vector<uint32_t> frames;  // Each frame's texels: its load list's first G_SETTIMG w1, or its record's data.
        int16_t start = 0;             // First frame (the target's own texels).
        bool forward = true;           // Step direction.
        float period = 0.0f;           // Seconds per step.
    };

    // A G_SETTILESIZE command of a scrolled texture's load list, with its w1 as converted (never changed).
    struct TexTileSize {
        uint32_t offset = 0;
        uint32_t w1 = 0;
    };

    // A texture scroll (2049 main 0x8011A840, kinds 9 and 10): the target's load list gets the tile origin uls (kind
    // 9) or ult (kind 10) = position / 4, halved per smaller mipmap tile; position advances `speed` per vblank.
    struct TexScroll {
        std::string target;
        std::vector<TexTileSize> tile_sizes;
        bool t = false;                // Kind 10: scroll t; kind 9: scroll s.
        int16_t position = 0;          // Start position (+4), wraps into [0, wrap) (+6).
        int16_t wrap = 0;
        int8_t speed = 0;              // +8, per vblank (1/60 s).
        int16_t rate = 1;              // +0xA, update interval in 1/30 s.
    };

    struct TexAnims {
        std::vector<TexFlipbook> flipbooks;
        std::vector<TexScroll> scrolls;
    };

    struct ConvertedTrack {
        std::vector<uint8_t> geometry;      // Asset 0x33 + slot.
        std::vector<uint8_t> placement;     // Asset 0x3F + slot.
        std::vector<uint8_t> collision;     // Asset 0x4B + slot.
        std::vector<uint8_t> path;          // Asset 0x57 + slot.
        std::vector<uint8_t> path_backward; // Asset 0x63 + slot.
        std::vector<uint8_t> pvs;           // The slot's visibility table: 16 bytes per camera region.
        uint8_t fog[3];                     // Fog colour, RGB.
        uint8_t pvs_count;                  // Camera regions under visibility control.
        std::vector<PathRecord> path_records;
        std::vector<SpinRecord> spin_records;
        std::vector<int16_t> demo_starts[2]; // Attract-mode start spine indices, forward and backward.
        TexAnims tex_anims;                  // Animated textures, as patch sites in `geometry`.
    };

    // The stunt arenas' k for convert_track: stunt arena n (1-4) is k = stunt_first + n - 1 (2049 track id 14 + n - 1).
    constexpr int stunt_first = 15;
    constexpr int stunt_count = 4;

    // rom2049: the big-endian Rush 2049 (USA) ROM. k: 2049 track id + 1, race tracks 1-6 or stunt arenas
    // stunt_first.. (which have no demo starts and one AI path, used both ways). prefix: the Rush 2 slot's track prefix
    // (e.g. "HAWAII", table 0x800C182C). shared_models: names of the models in Rush 2's shared assets 0x12 and 0x14,
    // which are left out of the merged geometry (rush2_shared_model_names). static_paths: also place every 2049
    // path-following object at its spawn nodes, as world-space top-level records after the sections (listed in
    // path_records). Returns false with a message in error if the track can't be converted.
    // Rush 2049 car `car` (1-13) as a Rush 2 car asset (asset 0x1D + type) for the Rush 2 car named `name`.
    // The Rocket ZX (car 3) also gets its exhaust flames, models ROKTFLAMEG1-3 of effects file 62.
    constexpr int rocket_car = 3;
    constexpr int effects_file = 62;
    bool convert_car(const std::vector<uint8_t>& rom2049, int car, const std::string& name, std::vector<uint8_t>& out,
                     std::string& error);

    bool convert_track(const std::vector<uint8_t>& rom2049, int k, const std::string& prefix,
                       const std::set<std::string>& shared_models, bool static_paths, ConvertedTrack& out,
                       std::string& error);

    // Names in a Rush 2 model container's name table, or an empty set if it isn't one.
    std::set<std::string> model_names(const std::vector<uint8_t>& rush2_container);

    // Rush 2's LZ decompressor (func_80003C6C): like Rush 2049's, but match positions are absolute in a 4KB ring
    // buffer. Returns false on truncated data.
    bool rush2_lz_decompress(const uint8_t* src, size_t src_size, std::vector<uint8_t>& out);

    // The models of Rush 2's shared assets 0x12 (LZ) and 0x14 (deflate), given rush2_rom (big-endian) and the two
    // assets' ROM offsets from the asset table at 0x800C185C.
    bool rush2_shared_model_names(const std::vector<uint8_t>& rush2_rom, uint32_t asset12_rom, uint32_t asset14_rom,
                                  std::set<std::string>& out);
}

#endif
