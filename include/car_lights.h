#ifndef __CAR_LIGHTS_H__
#define __CAR_LIGHTS_H__

#include <cstdint>
#include <vector>

#include "recomp.h"

// Car Lights (Graphics > Effects, src/car_lights.cpp, docs/graphics_effects.md#car-lights): on a night track every
// race car's headlamps and tail lamps light the road and what is round them from where the lamps are, and the lamps
// glow. The tail lamps brighten while the car brakes.
namespace rush2::car_lights {
    // The Graphics option (hot-swappable).
    void set_enabled(bool on);
    // Race car `car`'s brake lights lit this frame, by Rush 2's own rule (func_800599A0, rush2_calspeed_brake_lights).
    void set_braking(int car, bool lit);

    // A lamp given by hand (a mod car's .json "lights", converted from the glb's space by src/car_mods.cpp, and
    // assets/car_lamps.json): where it is in the car's own axes, in feet (x across, y up, z forward: the frame the
    // car's display lists are drawn in), and its lens's radius in feet.
    struct LampSpec {
        bool head = false;
        float at[3] = {};
        float size = 0.25f;
        bool lens = true;   // its lens glows: the bright texels within `size` of `at` (false: hidden lamps)
        bool has_color = false;
        float color[3] = {};  // the lens's color (0-255): the texels near it are the lens, not the brightest
    };
    // Mod car type `type`'s lamps from its .json (src/car_mods.cpp); an empty list leaves them to be found.
    void set_mod_lamps(int type, const std::vector<LampSpec>& lamps);
}

#endif
