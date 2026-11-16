#ifndef __CAR_FLASHERS_H__
#define __CAR_FLASHERS_H__

#include <vector>

// Roof flashers on mod cars (src/car_flashers.cpp, docs/car_mods.md#flashers): a new car's .json "flashers" are glow
// sprites on the car that pulse while it races, as the PlayStation SF Rush's police car's roof lights do
// (docs/sfrush_ps1.md#police-lights).
namespace rush2::car_flashers {
    constexpr int max_flashers = 4;   // per car
    struct Flasher {
        float at[3] = {};       // the car's own axes, feet (x across as the game's, y up, z forward)
        float rgb[3] = { 255.0f, 255.0f, 255.0f };
        float phase = 0.0f;     // where in the cycle it starts (0-1)
        float radius = 1.0f;    // the glow's radius, feet
    };
    struct Spec {
        std::vector<Flasher> flashers;
        float period = 0.5f;    // seconds: each flasher brightens from dark to full over it, then goes dark again
    };
    // Mod car type `type`'s flashers from its .json (src/car_mods.cpp); an empty list: none.
    void set_mod_flashers(int type, const Spec& spec);
}

#endif
