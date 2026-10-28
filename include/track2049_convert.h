#ifndef __TRACK2049_CONVERT_H__
#define __TRACK2049_CONVERT_H__

#include <array>
#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "rush2049_rom.h"

// Conversion of a Rush 2049 race track to the files of a Rush 2 track slot (src/rush2049/track2049_convert.cpp). 
// tools/rush2049/cpp_test checks its output with the Rush 2 format validators.
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

    // A placement record of a Rush 2049 object that a car knocks over: kind 2 props (CONE1, GASPUMP, RAT, RATCONE), signs
    // (kind 0, sub-kinds 1-2) and CACTUS. The record draws the 2049 model renamed X49<model>; poses are world space.
    struct PropRecord {
        int record;         // record index in the placement file
        int type;           // row of 2049's type table (main 0x80117530), or a custom track's prop_knock / prop_tip
        int direction;      // 0 both directions, 1 forward races only (_FW), 2 backward only (_BW)
        float m[9], pos[3]; // the object's pose
        float parent_m[9], parent_pos[3]; // its parent record's pose (identity and 0 for a top-level record)
    };

    // A custom track's props (PROP_KNOCK_*, PROP_TIP_*) react as these 2049 types, looked up by name in the type table
    // when the race starts (the mod's own model is drawn).
    constexpr int prop_knock = -2;      // CONE1: knocked flying, gone after 5 s
    constexpr int prop_tip = -3;        // STOPHIT: tips over and stays down

    // A battle arena's weapon or power-up pickup (WEPICON_*): drawn by a placement record of its model, which
    // src/rush2049/battle.cpp hides and shows. kind: 0-7 the weapons CANN, GATT, GREN, MINE, MISS, RAM, ROCK, SONC (2049's
    // order, docs/rush2049_research/battle.md), 8 HEAL, 9 INVS, 10 SHLD, 11 POWUP (a random power-up).
    struct PickupRecord {
        int record;     // record index in the placement file
        int kind;
        float pos[3];   // world position
    };
    // A custom arena's pickups (record -1) each take one of battle.cpp's renderer slots 60-159.
    constexpr int max_custom_pickups = 100;

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

    // A palette cycle (California Speed main 0x800B989C, kinds 0-6, docs/calspeed_research.md "Animated textures"):
    // every `period` vblanks the colors `first`..`last` of the target's palette are rotated, swapped or set.
    struct TexPalette {
        std::string target;
        uint32_t data = 0;             // Offset of the palette's colors (palette record +0x14).
        std::vector<uint16_t> colors;  // The colors as converted, entry 0 to the last one the cycle touches.
        int16_t first = 0;             // +4
        int16_t last = 0;              // +6
        int8_t step = 0;               // +8: kinds 5 and 6, the gap between the swapped runs.
        uint8_t kind = 0;              // +9: 0 rotate up, 2 down, 3/4 blink (set `blink` 0 / 1, then the other), 5 swap
                                       // two runs, 6 rotate three.
        int8_t period = 0;             // +0xA, in vblanks.
    };

    struct TexAnims {
        std::vector<TexFlipbook> flipbooks;
        std::vector<TexScroll> scrolls;
        std::vector<TexPalette> palettes;
        std::vector<uint16_t> blink[2];   // The 15 colors palette kinds 3 and 4 set.
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
        // A custom track's SKY mesh: drawn size / modeled size (under 1 when it is shrunk to fit s16 vertices). The
        // sky is placed at (1 - sky_scale) x the camera's height, so it looks as it would at its modeled size.
        float sky_scale = 1.0f;
        std::vector<PathRecord> path_records;
        std::vector<SpinRecord> spin_records;
        std::vector<PropRecord> prop_records;
        std::vector<PickupRecord> pickup_records;   // Battle arenas only.
        std::vector<int> pool_records;              // Battle arenas: placement records of the projectile and effect pool.
        std::vector<float> solid_triangles;         // Battle arenas: the collision's solid polygons, 9 floats a triangle.
        std::vector<int16_t> demo_starts[2]; // Attract-mode start spine indices, forward and backward.
        TexAnims tex_anims;                  // Animated textures, as patch sites in `geometry`.
        // Custom tracks' moving objects (src/track_mod/), in the layouts src/rush2049/track2049_movers.cpp reads from
        // a 2049 track: the paths as a 2049 file with a PTHD chunk, and a 2049 collision file holding only each
        // polygon's info word and the MOVER records. Empty for the 2049 tracks, whose own files are used.
        std::vector<uint8_t> mover_paths;
        std::vector<uint8_t> mover_collision;
    };

    // The battle arenas' k for convert_track: DM n (1-8) is k = battle_first + n - 1 (2049 track id 6 + n - 1).
    constexpr int battle_first = 7;
    constexpr int battle_count = 8;
    constexpr int battle_pool_size = 44;    // Placement records in a battle arena's projectile and effect pool.
    // The stunt arenas' k for convert_track: stunt arena n (1-4) is k = stunt_first + n - 1 (2049 track id 14 + n - 1).
    constexpr int stunt_first = 15;
    constexpr int stunt_count = 4;
    // The obstacle course's k (2049 track id 18).
    constexpr int obstacle = 19;

    // rom2049: the big-endian Rush 2049 (USA) ROM. k: 2049 track id + 1, race tracks 1-6, battle arenas battle_first.., stunt arenas stunt_first..
    // or the obstacle course (which have no demo starts and one AI path, used both ways; the obstacle course's runs
    // from its start to its finish). prefix: the Rush 2 slot's track prefix
    // (e.g. "HAWAII", table 0x800C182C). shared_models: names of the models in Rush 2's shared assets 0x12 and 0x14,
    // which are left out of the merged geometry (rush2_shared_model_names). static_paths: also place every 2049
    // path-following object at its spawn nodes, as world-space top-level records after the sections (listed in
    // path_records). Returns false with a message in error if the track can't be converted.
    // Rush 2049 car `car` (1-13) as a Rush 2 car asset (asset 0x1D + type) for the Rush 2 car named `name`.
    // The Rocket ZX (car 3) also gets its exhaust flames, models ROKTFLAMEG1-3 of effects file 62. The body also
    // becomes Rush 2's sheen layers, parts `name` + sheen_parts: D0_DFR1 / D2_DFR1 (the DECAL layer, car record
    // +0x3C) and D0_D2FR2 / D2_D2FR2 (the SHEEN layer, +0x50), drawn only when Car Sheen is All On.
    constexpr int rocket_car = 3;
    constexpr int effects_file = 62;
    constexpr int coins_file = 68;        // GOLDCOIN / SILVERCOIN models (race tracks' coins).
    constexpr const char* sheen_parts[] = { "D0_DFR1", "D2_DFR1", "D0_D2FR2", "D2_D2FR2" };
    bool convert_car(const rush2::rom2049::Source& rom2049, int car, const std::string& name, std::vector<uint8_t>& out,
                     std::string& error);
    // A car that is one body in a Rush 2 container (California Speed's cars: model `frame` and its textures) as a Rush
    // 2 car asset for the car type named `name`, the same way: `name`FRAME1 is the body, the other parts empty, and the
    // sheen layers made from it. Its textures are renamed `texture_prefix` + their name.
    bool convert_body_car(const std::vector<uint8_t>& container, const std::string& frame, const std::string& name,
                          const std::string& texture_prefix, std::vector<uint8_t>& out, std::string& error);
    // The sheen layers convert_body_car makes, added to a Rush 2 car container that already has the body `name`FRAME1
    // and Rush 2's other parts as empty models (a mod car: src/car_mods.cpp). False with a message on a bad container.
    bool add_body_sheen(std::vector<uint8_t>& container, const std::string& name, std::string& error);

    // Rush 2049's part models `sources` (ENGINE01G1.., TIRE01G1..; setup screen file 56) as a Rush 2 model container,
    // named `names`, and each one's center (the middle of its vertices, in model units).
    bool convert_parts(const rush2::rom2049::Source& rom2049, const std::vector<std::string>& sources,
                       const std::vector<std::string>& names,
                       std::vector<uint8_t>& out, std::vector<std::array<float, 3>>& centers, std::string& error);

    // Rush 2049 model file `file` (one holding OBHD objects: menus, effects, weapons, cars, a track's objects, tracks;
    // the list is in docs/asset_catalog.md) as a Rush 2 container with every object under its own 2049 name, its lists
    // as 2049 draws them (lit models keep their normals as vertex colors), for the model dump (src/asset_catalog.cpp).
    bool convert_file(const rush2::rom2049::Source& rom2049, int file, std::vector<uint8_t>& out, std::string& error);

    bool convert_track(const rush2::rom2049::Source& rom2049, int k, const std::string& prefix,
                       const std::set<std::string>& shared_models, bool static_paths, ConvertedTrack& out,
                       std::string& error);

    // Names in a Rush 2 model container's name table, or an empty set if it isn't one.
    std::set<std::string> model_names(const std::vector<uint8_t>& rush2_container);
    // A converted Rush 2 container's G_SETTILESIZE commands in the texture-load list of texture `name` (empty when it
    // has none, or its record points at texels); a scroll's patch sites. size: the texture's height (t) or width.
    std::vector<TexTileSize> texture_tile_sizes(const std::vector<uint8_t>& geometry, const std::string& name, bool t,
                                                int* size = nullptr);
    // The offset of palette `name`'s colors (its record +0x14) in a converted Rush 2 container, or 0.
    uint32_t palette_colors(const std::vector<uint8_t>& geometry, const std::string& name);

    // Rush 2's LZ decompressor (func_80003C6C): like Rush 2049's, but match positions are absolute in a 4KB ring
    // buffer. Returns false on truncated data.
    bool rush2_lz_decompress(const uint8_t* src, size_t src_size, std::vector<uint8_t>& out);

    // The models of Rush 2's shared assets 0x12 (LZ) and 0x14 (deflate), given rush2_rom (big-endian) and the two
    // assets' ROM offsets from the asset table at 0x800C185C.
    bool rush2_shared_model_names(const std::vector<uint8_t>& rush2_rom, uint32_t asset12_rom, uint32_t asset14_rom,
                                  std::set<std::string>& out);

    // Rush 2 model containers merged into one, as convert_track merges a 2049 track's files (California Speed's track
    // and shared object files, src/calspeed/calspeed_convert.cpp): every model under its own name (the first of a
    // name wins) or rename's, plus dummies (models drawing nothing) and dummy_textures (textures loading nothing),
    // names sorted for Rush 2's binary searches. Models in exclude are left out. A model's behaviour id is kinds'
    // entry for its output name, else default_kind (if >= 0), else its own. names gets the merged models' names.
    // picks: for a container (by its index in `containers`), only these models and these textures (source name ->
    // output name) are taken from it, e.g. a few effect models out of a game's shared effects file.
    struct Rush2Pick {
        std::set<std::string> models;
        std::map<std::string, std::string> textures;
    };
    struct Rush2Merge {
        std::map<std::string, std::string> rename;
        std::vector<std::string> dummies, dummy_textures;
        std::set<std::string> exclude;
        std::map<std::string, uint16_t> kinds;
        int default_kind = -1;
        std::map<size_t, Rush2Pick> picks;
    };
    bool merge_rush2_containers(const std::vector<const std::vector<uint8_t>*>& containers, const Rush2Merge& spec,
                                std::vector<uint8_t>& out, std::set<std::string>& names, std::string& error);

    // Rush 2049's car lighting colours of track k (docs/rush2049_research/collision.md#car-lighting), from its main
    // code: the 32 words of the track's table (0x8011AF90 + 4 (k - 1) points to it; 2049 reads past its 26) and the
    // colour used in the air (table index s32 0x8011B468 + 4 (k - 1)), 0xRRGGBBAA.
    bool car_light_table(const std::vector<uint8_t>& main_2049, int k, std::vector<uint32_t>& colours, uint32_t& air);
    // Where track k's artists painted a lighting colour, for the car light probes (src/car_probes_bake.cpp): every
    // collision polygon with a colour index (info bits 11-15) other than 0, not flagged 0x30 and not a moving one, as
    // world triangles (9 floats each), and each triangle's colour (0xRRGGBBAA). base: also the track's general level,
    // the area-weighted median colour of its floors (unpainted ones count as index 0): white on the day tracks, dark
    // on Presidio's night.
    bool car_light_paint(const rush2::rom2049::Source& rom, int k, std::vector<float>& tris, std::vector<uint32_t>& colours,
                         uint32_t* base = nullptr);
}

#endif
