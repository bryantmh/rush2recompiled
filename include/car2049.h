#ifndef __CAR2049_H__
#define __CAR2049_H__

#include <array>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "recomp.h"
#include "car_types.h"

namespace rush2::rom2049 {
    class Source;
}
namespace rush2::online::sync { class Walker; }

// Rush 2049 cars as Rush 2 car types 23-35 (src/rush2049/car2049.cpp); type 22 is Rush 2's "no car" marker. This file
// also keeps what every extra type (2049's, California Speed's 36-56 and mod cars 57-72, include/car_types.h) shares:
// the widened per-type tables, assets, side slots for the player records' options, denting and the car select's list.
namespace rush2::car2049 {
    constexpr int rush2_types = rush2::car_types::rush2_types;
    constexpr int car_count = rush2::car_types::count_2049;
    constexpr int first_type = rush2::car_types::first_2049;
    constexpr int end_type = first_type + car_count; // 36: past the 2049 types
    constexpr int types = rush2::car_types::types;   // 73: every type (the tables' width)

    // The car select's per-player car list, moved from 0x803CB368 to hold every type: car i of player p at
    // + 2 * i + p (count: s16 [2] at 0x803CB398).
    constexpr uint32_t car_list = rush2::car_types::car_list;
    // Appends the 2049 cars and California Speed's (when available) to player p's car list.
    void append_to_car_list(uint8_t* rdram, int player);
    // Whether car type `type` can be raced now: a Rush 2 car, or an extra one whose ROM is in and option on.
    bool type_available(int type);
    // Lowers player p's ENGINE choices on the 2049 cars to levels the unlock system offers them.
    void limit_engines(uint8_t* rdram, int player);
    // Puts player p's TIRES choices the unlock system doesn't offer them back to the car's default.
    void limit_tires(uint8_t* rdram, int player);
    // Swaps two players' rows (0-3) of the per-player car tables (src/players4.cpp's second car select round).
    void swap_player_rows(uint8_t* rdram, int a, int b);

    // Builds the per-type tables of every type (us.toml points the game at them). Call once at boot.
    void init_tables(uint8_t* rdram);
    // Builds the extended asset tables and serves the converted 2049 cars (assets 0x71 + n) and engine models (asset
    // parts_asset: models part_model_name(i), Rush 2049's ENGINE0<first_part_model + i>G1, then its TIRE01G1..TIRE04G1
    // as part_model_name(part_models + i)). Call once at boot.
    constexpr int parts_asset = 0x7E;
    constexpr int first_part_model = 1;
    constexpr int part_models = 5;
    constexpr int tire_models = 4;
    // Rush 2049's TIRES (0 RADIALS, 1 SLICKS, 2 PRO SLICKS, 3 ALL TERRAIN, 4 OFF ROAD): their names, and the model its
    // setup screen shows for one (tire % tire_models).
    const char* tire_name(int tire);
    // Rush 2049's ENGINE levels (setup row C): their names, and the model its setup screen shows for one
    // (level % part_models, as its overlay's part drawing at 0x8039E7F0 picks it).
    constexpr int engine_levels = 9;
    const char* engine_name(int level);
    std::string part_model_name(int i);
    std::array<float, 3> part_center(int i);
    // A player record's selected car (the byte at record + 0x31, with the 2049 types kept beside it).
    int saved_type(uint8_t* rdram, uint32_t at);
    void init_assets(uint8_t* rdram);
    // Converts the cars and part models of a Rush 2049 source into the disk cache (those not there yet) and reads them
    // into memory, so init_assets / check_source only install them. Any thread (src/prewarm.cpp, at launch).
    void prepare(const rush2::rom2049::Source& rom);
    // Fills the 2049 types' physics: descriptors, per-type table entries and boxes. Call after init_tables.
    void init_physics(uint8_t* rdram);
    // Once per frame: rebuilds the cars and their physics in the menus after the Rush 2049 source changed.
    void check_source(uint8_t* rdram);
    // The number of paint jobs (car select STYLEs) of a car type: a 2049 car from a Dreamcast disc has its own, any
    // other car 0.
    int paint_jobs(int type);
    // A Rush 2 car type drawn as one body, as a 2049 car is (a car mod's mesh, src/car_mods.cpp): its asset (0x1D +
    // type) becomes `car`, a car container whose <prefix>FRAME1 is the body and whose other parts are empty, dented
    // by the car's panel damage like a 2049 car's body. scuffed: the body's textures' damaged copies (file offset of
    // the pristine texels -> the copy), which a damaged panel's triangles draw with. Rush 2's stripes aren't stamped
    // on it. clear_body_car puts the type's own car back (`own`: the asset as it was before, empty = the ROM's).
    // A California Speed type (36-56) is drawn the same way, served as asset first_calspeed_asset + n.
    void set_body_car(uint8_t* rdram, int type, std::vector<uint8_t> car, const std::string& prefix,
                      const std::map<uint32_t, std::vector<uint8_t>>& scuffed);
    void clear_body_car(uint8_t* rdram, int type, const std::vector<uint8_t>& own);
    // A car mod livery's body (src/car_mods.cpp, docs/car_mods.md#liveries): model `prefix`FRAME1 of car container
    // `car` and its sheen layers, damaged as a one-body car's (set_body_car) by the panels of a car showing it (the copy
    // rush2::car_mods::livery_body names): dented, and each texture load of the body (lists at loads_start..loads_end,
    // which its lists call) inlined per panel and switched to its scuffed copy (`scuffed`: pristine texels -> copy);
    // the lists are rewritten in `car`. Returns its id (`id` again when given), -1 without vertices.
    // clear_livery_bodies drops them all.
    int add_livery_body(int id, std::vector<uint8_t>& car, const std::string& prefix,
                        const std::map<uint32_t, std::vector<uint8_t>>& scuffed, uint32_t loads_start, uint32_t loads_end);
    void clear_livery_bodies();
    // California Speed car n (type car_types::first_calspeed + n, src/calspeed/calspeed_cars.cpp): `car` is its body
    // (convert_body_car, parts named `prefix`, at most 7 characters), drawn and dented as set_body_car's, its damaged
    // panels scuffed along California Speed's paint ramps, and painted as California Speed paints it (func_800BAE2C):
    // `palette` (its CARPALETTE, 256 RGBA5551 big-endian) with MAIN and ACCENT as its two colors.
    void set_calspeed_car(uint8_t* rdram, int n, std::vector<uint8_t> car, const std::string& prefix,
                          const std::vector<uint8_t>& palette);
    // A car select name logo: a finished 128x32 CI8 image (texels rows bottom-up, 256-color RGBA16 big-endian palette)
    // or, when texels is empty, text lettered in the port's 5x7 logo font.
    struct NameLogo {
        std::string text;
        std::vector<uint8_t> texels, palette;
    };
    // California Speed's cars' car select logos, in order (src/calspeed/calspeed_logos.cpp).
    void set_calspeed_logos(uint8_t* rdram, const std::vector<NameLogo>& logos);
    // A car a mod adds (src/car_mods.cpp, docs/car_mods.md#new-cars): mod car n is type car_types::first_mod + n, drawn
    // and dented as set_body_car's and painted by Rush 2 (its textures are stand-ins in Rush 2's car palette). It
    // starts as a copy of Rush 2 car `base`: its physics descriptor (with the mod's wheel points), per-type table
    // entries (engine sound, tires, rims...), box and a player's options on it.
    // A mod car's color schemes: each a block of its body's lists between two G_NOOPs with these words (start, end);
    // the car's copy shows its STYLE's block and branches past the others (rush2_car49_paint).
    constexpr int max_schemes = 16;
    constexpr uint32_t scheme_marker(int scheme, bool end) {
        return 0x5C4E0000u | (uint32_t(scheme) << 1) | (end ? 1u : 0u);
    }
    struct ModCar {
        std::string name;              // the car select logo's text
        int base = 0;                  // Rush 2 type 0-21
        bool has_wheels = false;
        float wheels[4][3] = {};       // descriptor wheel points (feet): front right, front left, rear right, rear left
        float wheel_scale[2] = { 0.0f, 0.0f }; // front, rear wheel model scale (0: the base car's)
        bool has_box = false;
        float box[4] = {};             // front, rear, half width, height (feet)
        int main = -1, accent = -1;    // default MAIN and ACCENT COLOR (-1: the base car's)
        bool paint_main = true, paint_accent = true; // its textures take MAIN / ACCENT COLOR (car_mods sets)
        int schemes = 1;               // color schemes (whole liveries): more than 1 shows STYLE (paint_jobs)
        // A Rush 2049 car (0-12) whose physics it takes, as build_physics makes them (descriptor and per-type physics
        // values), in place of the base car's; -1: the base car's. Without Rush 2049 loaded, the base car's.
        int physics_from = -1;
        // Physics the .json's "physics" sets (car_mods.cpp read_new_car), over the base or physics_from car's.
        std::vector<std::pair<uint32_t, float>> desc_floats;   // descriptor offset (Rush 2's 0xEC layout), value
        std::vector<std::pair<uint32_t, float>> table_floats;  // per-type table (Rush 2 address), value in every row
        std::vector<std::pair<uint32_t, int>> table_bytes;     // per-type byte table (Rush 2 address), every row
        int top_gear = -1;             // descriptor +0xE0
        bool tires_2049 = false;       // Rush 2049's tire curves (front, rear) in place of the base car's
    };
    // Mod car n from the car container `car` (as set_body_car's), parts named `prefix` (at most 7 characters).
    void set_mod_car(uint8_t* rdram, int n, std::vector<uint8_t> car, const std::string& prefix,
                     const std::map<uint32_t, std::vector<uint8_t>>& scuffed, const ModCar& spec);
    // The Rush 2049 car (0-12) with this name ("BRUISER", "8-BALL", ...; case and spaces ignored), -1 if none.
    int car_index(const std::string& name);
    // Takes mod car n off the car select (its mod was turned off).
    void clear_mod_car(uint8_t* rdram, int n);
    // Mod car n's name, empty when no mod fills the slot.
    std::string mod_car_name(int n);
    // Online races (src/car_mods.cpp lend_slot, docs/online.md#mod-cars): a mod car's type number is part of the race's
    // state, so a raced mod car is in the same slot on every machine. copy_mod_car puts this machine's mod car `from`
    // (the same car, in another slot here) in slot n too; set_stand_in_mod_car puts a car this machine hasn't there:
    // `spec`'s physics, drawn as its base Rush 2 car. car_mods puts its own cars back after the race.
    void copy_mod_car(uint8_t* rdram, int n, int from);
    void set_stand_in_mod_car(uint8_t* rdram, int n, const ModCar& spec);
    // Lends slot n to the race (game thread, in the menus): copy_mod_car(from) when from >= 0, else the stand-in.
    // What the slot held is kept; return_mod_slots puts every lent slot back as it was.
    void lend_mod_slot(uint8_t* rdram, int n, int from, const ModCar& spec);
    // Puts back every lent slot, and the car physics data physics_online_sync took from the host.
    void return_mod_slots(uint8_t* rdram);
    // Online races' setup check (src/online/online_game.cpp): FNV-1a of `h` over what the physics read for a car of
    // `type` in per-type table row `row` (its physics tables' entries, descriptor and box).
    uint32_t physics_hash(uint8_t* rdram, int type, int row, uint32_t h);
    // An online race's resync part (include/online_sync.h): the physics data each race car runs on (physics_hash's,
    // its own descriptor and the tire curves it points at), so a car set up differently on a machine takes the host's.
    void physics_online_sync(rush2::online::sync::Walker& w);
    // Mod car n's spec (its physics, name and look settings) as bytes, empty for an empty slot, and back.
    std::vector<uint8_t> mod_car_spec(int n);
    bool read_mod_car_spec(const std::vector<uint8_t>& bytes, ModCar& out);
    // Whether car `type` has a MAIN COLOR (bit 0) and ACCENT COLOR (bit 1) its textures take: a mod car's own, 3 for
    // any other type. Mod cars have no stripes (their texture prefix matches none).
    int mod_car_paints(int type);
    constexpr int first_calspeed_asset = 0x80;
    constexpr int first_mod_asset = 0xA0;
    // A car type's asset index (func_80087E88's type + 0x1D, moved for the extra types).
    constexpr int car_asset(int type) {
        return type < first_type ? 0x1D + type : type < end_type ? 0x71 + type - first_type
             : type < rush2::car_types::first_mod ? first_calspeed_asset + type - end_type
                                                  : first_mod_asset + type - rush2::car_types::first_mod;
    }
    static_assert(first_calspeed_asset + rush2::car_types::count_calspeed <= first_mod_asset);
    static_assert(car_asset(types - 1) < rush2::car_types::asset_slots);
    // 2049 car k's scuffed panel copies (asset file offsets: pristine texels -> their scuffed copy), which its damage
    // points the body's loads at (rush2::catalog catalogs them as the panels' _D1 textures).
    std::map<uint32_t, uint32_t> scuffed_texels(int k);
    // 2049 car k's own palette (its CnPALETTE, 256 RGBA5551 entries big-endian) that rush2_car49_paint paints COLOR 1-3
    // into (entries 32-63, 64-95, 96-126); empty for a Dreamcast disc's car (paint jobs, no ramps) or before the cars
    // are loaded.
    std::vector<uint8_t> palette(int k);
    // The "Rush 2049 Cars" option (Games tab): whether the car select offers the 2049 cars.
    void set_option(bool enabled);
    // True when the option is on and the Rush 2049 ROM is available.
    bool available();
    // The name of 2049 car k (type first_type + k), as its car select logo shows it.
    const char* display_name(int k);
    // The "Rush 2049 Computer Cars" option (Games tab): where the computer cars may be 2049 cars.
    enum class DroneCars : uint32_t { AllTracks, Rush2049Tracks, Off };
    void set_drone_cars(DroneCars mode);
    // The "Car Speeds" option (Games tab). Rush 2049's cars and AI lanes are faster than Rush 2's; Rush2 runs every
    // car and lane at Rush 2's speed, Rush2049 at Rush 2049's (docs/rush2049_research/cars.md §10).
    enum class SpeedMode : uint32_t { Rush2, Rush2049 };
    void set_speed_mode(SpeedMode mode);
    // The "Accurate Car Stats" option (General tab): the car select's bars come from the race physics (on) or from Rush
    // 2's own formulas (off).
    void set_accurate_bars(bool on);
    // Works out the Accurate Car Stats bars' ranges into the disk cache before the game starts, on a memory image of
    // its own (the launch's build pass, src/prewarm.cpp). Any thread; the ROMs loaded.
    void prepare_bar_ranges();
    // Works them out again after a menu change of their inputs, from the main loop (each frame).
    void prepare_bars(uint8_t* rdram, recomp_context* ctx);
    // The "Torque Rebalance" option (General tab): raises the HIGH torque curve's top end and lowers LOW's, so HIGH
    // trades launch for top speed (on), or keeps Rush 2's curves (off).
    void set_torque_rebalance(bool on);
    // An AI lane speed (u8 mph) of a Rush 2049 path (rush2049_path) or of any other, in the current mode.
    int map_lane_speed(int speed, bool rush2049_path);
    // How much the current mode raises Rush 2's mean lane speed (1 with Rush 2 speeds).
    float rush2_lane_scale();
    // The engine sound (0-9) a car of this type plays for ENGINE row value `value`: the value itself for a Rush 2
    // car, the type's default sound for a 2049 car (whose ENGINE row is its power level).
    int engine_sound(uint8_t* rdram, int type, int value);
    // Rush 2049's engine sound (src/rush2049/engine2049.cpp): per ENGINE level (0-5), up to two looping layers (table
    // 0x8010FD80), each pitched 1 + (rpm - base) / span and with a volume running through three rpm points.
    struct EngineLayer {
        int sound = -1; // 2049 sound effect id, or -1 for none
        float base = 0, span = 1;
        float rpm[3] = {};
        float volume[3] = {};
    };
    // The ENGINE level of the car whose physics struct is at `car` (its player's choice, or the drone setup's), or -1
    // if it isn't a 2049 car.
    int engine_level(uint8_t* rdram, uint32_t car);
    // ENGINE level `level`'s layers; false without the 2049 ROM.
    bool engine_layers(int level, EngineLayer out[2]);
    // Forgets the 2049 car options and selected car kept for the player record at `record` (a deleted player), so a
    // player created in its place starts fresh.
    void forget_record(uint8_t* rdram, uint32_t record);
    // The player record address a 2049 type's side-slot address stands for (record + 0x584 + type * 13 + the byte in
    // the type's block); any other address is returned as it is.
    uint32_t record_address(uint8_t* rdram, uint32_t addr);
    // Online races (src/online/online_game.cpp): a player's 2049 car options travel with their profile. side_block is
    // the side slot of the player record at `record` (one in the save area, made if missing; empty otherwise), and
    // set_guest_side_block gives the profile copy at `record` (outside the save area: race player `guest`'s, 0-7) those
    // options for the race, or a new record's for an empty block, without saving them. clear_guest_side_blocks forgets
    // them all.
    std::vector<uint8_t> side_block(uint8_t* rdram, uint32_t record);
    void set_guest_side_block(uint8_t* rdram, int guest, uint32_t record, const std::vector<uint8_t>& block);
    void clear_guest_side_blocks();
    // Animates the loaded 2049 cars' effects (the Rocket ZX's exhaust flames). Called while models are drawn; it
    // runs at most once per 1/60 s.
    void animate(uint8_t* rdram);
    // Called before func_8007AA48 draws model display list `dl`: queues the Rocket ZX's flames when it is the body.
    void draw_model(uint8_t* rdram, uint32_t dl);
    // Called last before func_8007AA48 emits the G_DL to $fp ($s7 = the node; `dl` = the model's own display list,
    // before other hooks swapped it): Rush 2049's car lighting. A 2049 car body sets its PRIM (white, or the colour of
    // the road under it on a 2049 track); on a 2049 track any other car part is drawn from a copy of its list whose
    // combiners multiply by that colour (through ENV and PRIM).
    void light_model(uint8_t* rdram, recomp_context* ctx, uint32_t dl);

    // Car sheen (Graphics > Car Sheen): Rush 2's translucent sheen layers over the cars. Original draws them on the
    // Rush 2 cars, All Off on none, All On on the Rush 2049 cars too (layers made from their bodies). Cube Map has
    // every car's SHEEN layers draw as the paint reflecting the scene around the car (RT64's car reflections,
    // gEXSetCarReflection) and drops the DECAL layers; the car select and the shop, where there is no scene to
    // reflect, draw as All On.
    enum class Sheen : uint32_t { Original, AllOff, AllOn, CubeMap };
    void set_sheen(Sheen mode);
    // Before func_8007AA48 emits the G_DL ($s7 = the node): drops a sheen layer the settings hide and marks the ones
    // drawn as reflections.
    void sheen_model(uint8_t* rdram, recomp_context* ctx);
}

#endif
