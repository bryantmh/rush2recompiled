#ifndef __SURFACE_FX_H__
#define __SURFACE_FX_H__

#include <array>
#include <cstdint>

// The wheel surface effects of another game's tracks raced in Rush 2 (src/surface_fx.cpp,
// docs/calspeed_research.md#4-surface-types).
//
// A collision polygon's wheel material (POLY flags bits 4-7) drives only effects in Rush 2: the dust, the skid sounds,
// the wheel particles, the tire marks and the stunt scoring, which know materials 0 (road), 1 (dirt: DUST), 2 (water:
// SPLASH) and 8 (airborne). A converted track keeps its own game's materials; while it is raced, its game's Rules say
// what each of them does, and Rush 2's own code sees the material each acts like. California Speed's are here; SF
// Rush's and Rush 2049's tracks keep Rush 2's (their materials are Rush 2's numbers) until they need their own.
namespace rush2::surface_fx {
    // The particle a wheel throws while the car's dust bits are set (Rush 2's particle_emitter_spawn_8008270C).
    enum class Particle : uint8_t {
        Rush2,   // whatever Rush 2 throws for the `like` material
        Dust,    // DUST (Rush 2's material 1)
        Smoke,   // SMOKE / SMOKE_FAST by speed (Rush 2's road)
        None,
    };

    struct Material {
        uint8_t like = 0;                 // the Rush 2 material its code sees: 0 road, 1 dirt, 2 water
        Particle particle = Particle::Rush2;
        bool marks = false;               // a skidding wheel lays tire marks on it
        uint32_t mark_rgb = 0;            // their color, 0xRRGGBB
        bool spray = false;               // a turning wheel throws the water spray models
    };

    struct Rules {
        std::array<Material, 16> materials{};
        const char* mark_texture = nullptr;              // the tire marks' texture, else Rush 2's SKID04
        const char* spray_models[2] = { nullptr, nullptr };   // right wheels (0, 2), left wheels (1, 3)
        const char* spray_frames[4] = {};                // the spray's texture, one frame a game frame (frame & 3)
        uint32_t spray_rgba = 0xFFFFFFFF;                // the spray nodes' primitive color
    };
}

#endif
