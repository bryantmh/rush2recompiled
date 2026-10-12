#ifndef __STEAM_HAPTICS_H__
#define __STEAM_HAPTICS_H__

#include <string>

#include "dualsense_haptics.h"

// Steam Controller (2026, "Triton") haptics (src/steam_haptics.cpp): plays Rumble Mode > Enhanced's effects on the
// controller's grip actuators as tones, like a DualSense's voices (include/dualsense_haptics.h), instead of SDL's
// rumble, which drives them like old motors with one speed per side, so every effect felt the same. Wired (28DE:1302),
// Bluetooth (1303) and through the puck (1304/1305). One controller at a time.
namespace rush2::steam_haptics {
    // Whether a controller's vendor and product are a Steam Controller that takes tone reports.
    bool is_supported(unsigned vendor, unsigned product);
    // The controller to play on, by its SDL device path (SDL_GameControllerPath), or "" for none. A background thread
    // opens it and closes it again (and silences it) when this changes.
    void set_device(const std::string& path);
    // Whether the device is open and taking reports.
    bool active();
    // Each side's thump, buzz and grind levels and the voices' pitches (the player's profile's), as for the DualSense.
    void set_levels(const rush2::haptics::Levels& levels, float thump_hz, float buzz_hz, float grind_hz);
}

#endif
