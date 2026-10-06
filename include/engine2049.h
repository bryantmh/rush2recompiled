#ifndef __ENGINE2049_H__
#define __ENGINE2049_H__

#include <cstdint>

#include "recomp.h"

// Rush 2049's engine sounds for the 2049 cars (src/engine2049.cpp).
namespace rush2::engine2049 {
    // The car select's ENGINE row rev (src/engine_preview.cpp) on a 2049 car: starts ENGINE level `level`'s sound on
    // local player slot `slot` (0-1), sets its rpm and throttle (0-1), and stops it.
    void preview_start(uint8_t* rdram, int slot, int level);
    void preview_update(uint8_t* rdram, int slot, float rpm, float throttle);
    void preview_stop(int slot);
    // Rush 2049's engine law (func_800E0050) for ENGINE level `level` at `rpm` (2049's car +0x7D0, rpm x 0.9) and
    // `load` (+0x404, the engine torque): each layer's sound (-1 none), pitch and volume before the per-car scale
    // (the local player's 0.8 - 0.05 x players, other cars' 0.75). False without the 2049 ROM.
    struct LayerMix {
        int sound = -1;
        float pitch = 1.0f;
        float volume = 0.0f;
    };
    bool layer_mix(int level, float rpm, float load, LayerMix out[2]);
    // Set while src/engine_preview.cpp starts a Rush 2 engine itself, so the race's engine start hook leaves it be.
    void set_preview_call(bool on);
    // The unlock system's shop (src/unlocks_shop.cpp), which replaces the car select's frame: keeps the rev going
    // (call every frame), revs ENGINE level `level` on player 1's slot as the ENGINE row does, and how far the rev's
    // rpm is from idle (0) to its peak (1).
    void shop_frame();
    void shop_rev(uint8_t* rdram, recomp_context* ctx, int level);
    float shop_rev_amount();
}

#endif
