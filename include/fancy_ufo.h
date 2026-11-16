#ifndef __FANCY_UFO_H__
#define __FANCY_UFO_H__

#include <cstdint>
#include <vector>

#include "recomp.h"

namespace rush2::ui {
    class OptionsPage;
}

// Fancy UFO (Games tab, SF Rush PlayStation; src/sfrush_ps1/fancy_ufo.cpp, docs/sfrush_ps1.md#fancy-ufo): the
// PlayStation SF Rush mod's UFO (the car "PS1UFO") gets
//   - a starship ENGINE (WARP CORE, ENGINE value 10) and HORN (HAILING, HORN value 9) on its car select rows, its
//     defaults, played by src/car_engines.cpp from sounds made here;
//   - a hover glow: a beam of light under each of its three pods, with rings running down them;
//   - thruster jets from its rim while its hidden wings steer it in the air.
namespace rush2::fancy_ufo {
    // The UFO's size in the converted mod (src/sfrush_ps1/ps1_cars.cpp): the scale of every PS1 car (car_size, the
    // PS1's drawing scale to Rush 2's), 10.2 ft across, past Rush 2's width cap the other oversized cars are shrunk to. The glow and jets
    // here were laid out on the UFO shrunk to that cap (6.625 ft of 10.204, a fit of 0.649); `size` scales them to
    // its size now: across and along about its origin, and up about the axle height, 3.6 (1/16 ft), which the
    // converter keeps its wheel points at.
    constexpr double ps1_fit = 1.0;
    constexpr float size = float(ps1_fit / (6.625 / 10.204));
    constexpr float axle_y = 3.6f;

    constexpr int engine_value = 10;   // the ENGINE row's WARP CORE (Rush 2's engines are 0-9)
    constexpr int horn_value = 9;      // the HORN row's HAILING (Rush 2's horns are 0-8)

    // The option, in the Games tab's config (before recompui::config::finalize()), and its row in the SF Rush
    // PlayStation section.
    void init_config();
    void add_option(rush2::ui::OptionsPage* page);
    // Whether the option is on. Any thread.
    bool enabled();
    // Whether car type `type` is the PlayStation UFO. Any thread.
    bool is_ufo(int type);

    // The sounds (mono 16-bit at sound_rate): the engine's two looped layers (a hum and a whine) and the horn. Made
    // once on first use (well under a millisecond each).
    constexpr uint32_t sound_rate = 32000;
    const std::vector<int16_t>& engine_hum();
    const std::vector<int16_t>& engine_whine();
    const std::vector<int16_t>& horn();
    // The engine layers' pitch and volume for `drive` (0 idle .. 1 flat out).
    struct Layer {
        float pitch, volume;
    };
    void engine_mix(float drive, Layer out[2]);

    // Thruster jets: where the rim's four nozzles are, in the car's axes (feet:
    // right, up, forward).
    constexpr int thrusters = 4;
    void thruster(int i, float out[3]);

    // Car select ENGINE row (src/rush2049/car2049.cpp rush2_car49_engine_value): the UFO's next value from `before`
    // (stepping through WARP CORE too), or -1 when the row isn't the UFO's with the option on. $a1 the step, $a3 the
    // type, $s2 the engine Rush 2 skips.
    int engine_step(uint8_t* rdram, recomp_context* ctx, int before);
    // The car select's value text for the UFO's own ENGINE / HORN value (an RDRAM string), 0 for any other value.
    uint32_t label(uint8_t* rdram, int type, bool horn_row, int value);
    // Forgets the picks kept for the player record at `record` (a deleted player, src/pak.cpp).
    void forget_record(uint8_t* rdram, uint32_t record);
}

#endif
