#ifndef __LIGHT_BAR_H__
#define __LIGHT_BAR_H__

#include <cstdint>

// Controller light bars in the car colors (src/light_bar.cpp). Backported from the PS5 port
// (github.com/Marice/rush2-ps5, ps5/ps5_platform.cpp).
namespace rush2::light_bar {
    // The color (0xRRGGBBAA) the light bar of the controller on a port should show from the next update on, until
    // another color is set. Any thread.
    void set(int port, uint32_t rgba);
    // Sets a port's light bar to a car's paint color (include/paint_colors.h), unless the car has paint jobs instead
    // of colors (a Dreamcast car, whose MAIN row is its STYLE). Any thread.
    void set_car(int port, int type, int color);
    // Sends the colors that changed, also to a controller that took the port since. Once per VI, from the VI thread.
    void update();
}

#endif
