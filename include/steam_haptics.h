#ifndef __STEAM_HAPTICS_H__
#define __STEAM_HAPTICS_H__

#include <string>
#include <vector>

#include "dualsense_haptics.h"

// Steam Controller (2026, "Triton") haptics (src/steam_haptics.cpp): plays Rumble Mode > Enhanced's
// effects on Valve's haptic actuators as tones, like a DualSense's voices (include/dualsense_haptics.h), instead of
// SDL's rumble, which drives them like old motors with one speed per side, so every effect felt the same. The Steam
// Controller wired (28DE:1302), over Bluetooth (1303) and through the puck (1304/1305); the Steam Deck keeps SDL's
// rumble (its tones play on the trackpads). Every connected one plays its own. Windows and Linux (hidraw).
namespace rush2::steam_haptics {
    struct Controller {
        std::string path;     // SDL device path (SDL_GameControllerPath).
        unsigned product = 0; // USB product ID.
    };
    // Whether a controller's vendor and product take tones.
    bool is_supported(unsigned vendor, unsigned product);
    // The controllers that want tones. Background threads open each one and silence and close it a few seconds after
    // it's no longer listed.
    void set_controllers(const std::vector<Controller>& controllers);
    // Whether a controller is open and has taken a report (a device that can't, such as one Wine shows through its
    // generic driver, never is, so it keeps SDL's rumble).
    bool active(const std::string& path);
    // A controller's thump, buzz and grind levels per side and the voices' pitches (its player's profile's), as for the
    // DualSense.
    void set_levels(const std::string& path, const rush2::haptics::Levels& levels, float thump_hz, float buzz_hz,
                    float grind_hz);
}

#endif
