#pragma once
// Falling snow (src/snowfall.cpp, docs/custom_tracks.md#snowfall) and the snowflakes over the SNOW versions' track
// select dioramas, both under Graphics > Falling Snow.

#include <cstdint>

namespace rush2::snowfall {
    // Graphics > Falling Snow (src/config.cpp): read every frame, so a change shows at once.
    void set_enabled(bool on);
    bool enabled();

    // A diorama's snowflakes sit in its display list between two G_NOOPs tagged with these (w1): the track select
    // branches from the first past the second while Falling Snow is off (src/rush2049/track2049_menu.cpp
    // show_diorama_snow). The N64 SF Rush SNOW models (src/rush1/track1_convert.cpp) and a custom diorama's
    // SNOWFLAKES material (src/track_mod/track_mod_convert.cpp) are tagged.
    constexpr uint32_t flakes_start_tag = 0x534E4F57;   // "SNOW"
    constexpr uint32_t flakes_end_tag = 0x534E4F45;     // "SNOE"
}
