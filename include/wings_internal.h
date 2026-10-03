#ifndef __WINGS_INTERNAL_H__
#define __WINGS_INTERNAL_H__

// Shared between the src/wings*.cpp files.

#include <cstddef>
#include <cstdint>
#include <vector>

#include "recomp.h"
#include "wings.h"

namespace rush2::wings {
    constexpr int max_cars = 8;

    // Rush 2049's LZ decompressor (src/wings_rom.cpp). Returns false on truncated data.
    bool lz_decompress(const uint8_t* src, size_t src_size, std::vector<uint8_t>& out);
    // Decompressed Rush 2049 file 77, the wing and flame models.
    bool read_wing_model_file(const std::vector<uint8_t>& rom, std::vector<uint8_t>& out);

    // Controller Setup WINGS row (src/wings_menu.cpp).
    void load_controls();
    // True if player (0 or 1, the game's player struct index) holds the WINGS button and the game isn't paused.
    bool button_held(uint8_t* rdram, int player);

    // Wing state (src/wings_state.cpp).
    struct Pose {
        int style;            // 0-2
        float slide;          // Sideways slide of each wing, world units.
        float height;         // Height above the car's origin, world units.
        float tilt;           // Rotation about the car's X axis, radians.
        bool flames;
        float flame_scale[2]; // Flame stretch along Y, left and right.
    };
    // The wing pose of car index (0-7), or false if its wings are away.
    bool get_pose(uint8_t* rdram, int index, Pose& out);
    uint32_t car_struct(int index);
    void set_player_style(int player, int style);

    // Rendering (src/wings_render.cpp). Called from the model draw hook.
    void draw_car_body(uint8_t* rdram, recomp_context* ctx);

    // Sound (src/wings_sound.cpp).
    void reload_sound();
    void set_sound(int car, bool playing);
}

extern "C" {
    // The view index and scene graph generation of the node being drawn (src/interpolation.cpp).
    void rush2_interp_get_generation(uint32_t* view, uint32_t* gen);
}

#endif
