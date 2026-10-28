#ifndef __CONTROL_PROFILES_H__
#define __CONTROL_PROFILES_H__

#include <cstdint>
#include <string>
#include <vector>

// Control and rumble settings per game save profile (src/control_profiles.cpp), edited in the Controls tab
// (src/controls_tab.cpp). Each port uses the settings of the profile its player picked in the game (Select Player),
// the no-profile settings without one. Button bindings aren't here: they stay per port, in the game's Controller Setup
// screen (src/controls.cpp).
namespace rush2::control_profiles {
    enum class SteeringResponse : uint32_t { Original, Balanced, Linear };
    enum class ReverseControl : uint32_t { Gear, Hold };
    // Original: the game's own rumble, on both motors at one level. Automatic (shown as Enhanced): impacts a short
    // heavy thump, wall grinding a rough rasp and tire screech a lighter buzz, on every controller, and by side on
    // matched motors: a DualSense's actuators (include/dualsense_haptics.h), an Xbox One/Series pad's trigger motors, a
    // Steam Controller's or Steam Deck's two haptics. Uneven grip motors play no sides (src/input.cpp, RumbleOutput).
    // Directional was a separate choice when Automatic left other controllers on Original; it's read back as
    // Automatic. Older versions also saved "rumble_effects" (Distinct, or Uniform to rumble every effect alike) and a
    // left/right "balance"; they're ignored now.
    enum class RumbleMode : uint32_t { Automatic, Original, Directional };

    struct Settings {
        SteeringResponse steering = SteeringResponse::Original;
        ReverseControl reverse = ReverseControl::Gear;
        float deadzone = 5.0f;          // Stick deadzone during races, percent of the stick's travel.
        float rumble_strength = 100.0f; // Percent; 0 turns rumble off.
        RumbleMode rumble_mode = RumbleMode::Automatic;
        // Directional tuning, percent.
        float crash_strength = 100.0f;  // The impact thump.
        float screech_strength = 70.0f; // Tire screech.
        float grind_strength = 85.0f;   // Grinding along a wall.
        float far_side_cut = 100.0f;    // How much of the far side a hit square on to one side takes away.
        // Unused since rumble motors play the screech and grind as steady levels (src/input.cpp); kept so saved
        // settings read back unchanged.
        float screech_chatter = 12.0f;
        float grind_roughness = 70.0f;
        // DualSense haptics: the voices' pitch, Hz.
        float thump_hz = 55.0f;
        float buzz_hz = 170.0f;
        float grind_hz = 95.0f;

        // The steering exponent applied before the game's cube of the stick (1 keeps it, 1/3 makes it linear).
        float steering_exponent() const;
    };

    // Loads the saved settings, the first time moving the old global settings in as every profile's starting point.
    void load();

    // A profile's settings ("" = players without a profile); the defaults until it's changed.
    Settings get(const std::string& profile);
    // Changes and saves a profile's settings.
    void set(const std::string& profile, const Settings& settings);
    // The steering profiles start from until their settings are changed, set by the General tab's New Profile Steering
    // (a setup screen rule, src/config.cpp). Setting it saves it.
    SteeringResponse default_steering();
    void set_default_steering(SteeringResponse steering);

    // Game thread, every frame: which profile each port's player uses.
    void update_ports(uint8_t* rdram);
    // A port's settings now (its player's profile's, or the no-profile ones). Any thread.
    Settings port_settings(int port);
    // A port's profile name ("" without one).
    std::string port_profile(int port);

    // Profiles to choose from: the game's (in pak order, from rush2::collectibles::progress), then ones with saved
    // settings, then "" for players without a profile.
    std::vector<std::string> profiles();

    // The profile last picked in the Controls tab's profile picker, kept across launches; false if none was picked.
    bool edited_profile(std::string& out);
    void set_edited_profile(const std::string& profile);
}

#endif
