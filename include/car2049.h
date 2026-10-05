#ifndef __CAR2049_H__
#define __CAR2049_H__

#include <cstdint>

// Rush 2049 cars as Rush 2 car types 23-35 (src/car2049.cpp); type 22 is Rush 2's "no car" marker.
namespace rush2::car2049 {
    constexpr int rush2_types = 22;
    constexpr int car_count = 13;
    constexpr int first_type = 23;
    constexpr int types = first_type + car_count; // 36

    // The car select's per-player car list, moved from 0x803CB368 to hold 36 entries: car i of player p at
    // + 2 * i + p (count: s16 [2] at 0x803CB398).
    constexpr uint32_t car_list = 0x80222000;
    // Appends the 2049 cars (when available) to player p's car list.
    void append_to_car_list(uint8_t* rdram, int player);
    // Swaps two players' rows (0-3) of the per-player car tables (src/players4.cpp's second car select round).
    void swap_player_rows(uint8_t* rdram, int a, int b);

    // Builds the 36-entry per-type tables (us.toml points the game at them). Call once at boot.
    void init_tables(uint8_t* rdram);
    // Builds the extended asset tables and serves the converted 2049 cars (assets 0x71 + n). Call once at boot.
    void init_assets(uint8_t* rdram);
    // Fills the 2049 types' physics: descriptors, per-type table entries and boxes. Call after init_tables.
    void init_physics(uint8_t* rdram);
    // The "Rush 2049 Cars" option (Games tab): whether the car select offers the 2049 cars.
    void set_option(bool enabled);
    // True when the option is on and the Rush 2049 ROM is available.
    bool available();
    // The "Rush 2049 Computer Cars" option (Games tab): where the computer cars may be 2049 cars.
    enum class DroneCars : uint32_t { AllTracks, Rush2049Tracks, Off };
    void set_drone_cars(DroneCars mode);
    // The "Car Speeds" option (Games tab). Rush 2049's cars and AI lanes are faster than Rush 2's; Rush2 runs every
    // car and lane at Rush 2's speed, Rush2049 at Rush 2049's (docs/rush2049_research/cars.md §10).
    enum class SpeedMode : uint32_t { Rush2, Rush2049 };
    void set_speed_mode(SpeedMode mode);
    // An AI lane speed (u8 mph) of a Rush 2049 path (rush2049_path) or of any other, in the current mode.
    int map_lane_speed(int speed, bool rush2049_path);
    // How much the current mode raises Rush 2's mean lane speed (1 with Rush 2 speeds).
    float rush2_lane_scale();
    // The engine sound (0-9) a car of this type plays for ENGINE row value `value`: the value itself for a Rush 2
    // car, the type's default sound for a 2049 car (whose ENGINE row is its power level).
    int engine_sound(uint8_t* rdram, int type, int value);
    // Rush 2049's engine sound (src/engine2049.cpp): per ENGINE level (0-5), up to two looping layers (table
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
    // Animates the loaded 2049 cars' effects (the Rocket ZX's exhaust flames). Called while models are drawn; it
    // runs at most once per 1/60 s.
    void animate(uint8_t* rdram);
}

#endif
