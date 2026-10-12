#pragma once

#include <string>
#include <vector>

// DualSense haptics (src/dualsense_haptics.cpp): drives a DualSense's left and right voice-coil actuators directly,
// for Rumble Mode > Enhanced. SDL's rumble call puts a DualSense in its PS4-compatible mode, where the firmware makes
// the left actuator feel like a heavy motor and the right like a light one; the real actuators are a matched pair fed
// as channels 3 and 4 of the controller's own 4-channel sound device. Only over USB (Bluetooth has no sound device).
// Every DualSense plays on its own sound device. Windows only (the releases are).
namespace rush2::haptics {
    // Strength, 0 to 1, of each actuator's voices, [0] left and [1] right: a low thump (hits), a rough higher buzz
    // (tire screech) and a grinding rasp (scraping along a wall).
    struct Levels {
        float thump[2] = {};
        float buzz[2] = {};
        float grind[2] = {};
    };
    // The DualSenses that want haptics, by SDL device path (SDL_GameControllerPath). Background threads open each
    // one's sound device and close it a few seconds after it's no longer listed.
    void set_controllers(const std::vector<std::string>& paths);
    // Whether a DualSense's sound device is open and playing its levels.
    bool active(const std::string& path);
    // A DualSense's levels and the pitch of its thump, buzz and grind voices, Hz (its player's profile's).
    void set_levels(const std::string& path, const Levels& levels, float thump_hz, float buzz_hz, float grind_hz);
}
