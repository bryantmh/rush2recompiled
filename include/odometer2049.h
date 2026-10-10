#ifndef __ODOMETER2049_H__
#define __ODOMETER2049_H__

#include <cstdint>

// Rush 2049's odometer (src/rush2049/odometer.cpp): four rolling digits under each player's race time with the miles
// (km with the speedometer in km/h) their car has driven this race. The Odometer option is in the Games tab's Rush 2049
// section; src/hud.cpp places the digits.
namespace rush2::odometer2049 {
    // Drawn size in 4:3 screen pixels at full scale, and the gap to the element it is stacked on.
    constexpr int width = 32;
    constexpr int height = 8;
    constexpr int gap = 2;

    void set_option(bool on);
    // The option is on and Rush 2049 is loaded.
    bool enabled();
    // A race is set up (or restarted): the distances start over.
    void reset();
    // Once per drawn HUD: adds the distance each player's car moved since the last frame.
    void update(uint8_t* rdram);
    // Draws player's odometer with its top left corner at (x, y), `scale` times its full size, anchored at `anchor`
    // (rush2::hud::set_anchor).
    void draw(uint8_t* rdram, int player, float x, float y, float scale, float anchor);
}

#endif
