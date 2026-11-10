#ifndef __CAR_LIGHTS_H__
#define __CAR_LIGHTS_H__

#include <cstdint>

#include "recomp.h"

// Car Lights (Graphics > Effects, src/car_lights.cpp, docs/graphics_effects.md#car-lights): on a night track, the race
// cars' brake lights shine red on what is round them while they are lit.
namespace rush2::car_lights {
    // The Graphics option (hot-swappable).
    void set_enabled(bool on);
    // Race car `car`'s brake lights lit this frame, by Rush 2's own rule (func_800599A0, rush2_calspeed_brake_lights).
    void set_braking(int car, bool lit);
    // The model draw hook (after rush2::car2049::light_model, src/widescreen.cpp): a lit car's body part drawn with its
    // lit tail light texels again on top, unshaded, so the lenses glow on a dark track. ctx->r30 the list to draw.
    void model_draw(uint8_t* rdram, recomp_context* ctx, uint32_t dl);
}

#endif
