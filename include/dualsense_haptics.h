#pragma once

// DualSense haptics (src/dualsense_haptics.cpp): drives a DualSense's left and right voice-coil actuators directly,
// for Rumble Mode > Directional. SDL's rumble call puts a DualSense in its PS4-compatible mode, where the firmware makes
// the left actuator feel like a heavy motor and the right like a light one; the real actuators are a matched pair fed
// as channels 3 and 4 of the controller's own 4-channel sound device. Only over USB (Bluetooth has no sound device), and
// for one DualSense: the first sound device named like one.
namespace rush2::haptics {
    // Whether a port wants haptics. A background thread opens the DualSense's sound device while this is set and closes
    // it a few seconds after it isn't.
    void set_wanted(bool wanted);
    // Whether the DualSense's sound device is open and playing set_levels.
    bool active();
    // Strength, 0 to 1, of each actuator's voices, [0] left and [1] right: a low thump (hits, and all of Rumble
    // Effects > Uniform), a rough higher buzz (tire screech) and a grinding rasp (scraping along a wall).
    struct Levels {
        float thump[2] = {};
        float buzz[2] = {};
        float grind[2] = {};
    };
    void set_levels(const Levels& levels);
    // The pitch of the thump, buzz and grind voices, Hz (the haptics player's profile's).
    void set_tones(float thump_hz, float buzz_hz, float grind_hz);
}
