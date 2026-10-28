// Control and rumble settings per game save profile (include/control_profiles.h).
//
// Kept in players.json's "control_profiles" section (include/data_files.h):
//   { "version": 1, "defaults": {settings}, "profiles": [ { "name": "BOB", "key": "424f42", "settings": {settings} } ],
//     "edited": "424f42" }
// A profile is found by its name's bytes in hex ("key"; names can hold bytes JSON strings can't), "" for players
// without a profile. "defaults" is what a profile starts from until its settings are changed. "edited" is the profile
// last picked in the Controls tab (absent until one is picked), which the tab opens on.
//
// Before this, steering, reverse, the stick deadzone and rumble were global options in general.json. The first load
// (no section yet) reads them from there into "defaults", so every profile starts with what the player had.

#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>

#include "json/json.hpp"

#include "librecomp/game.hpp"

#include "collectibles.h"
#include "control_profiles.h"
#include "data_files.h"
#include "rush2.h"

namespace {
    using namespace rush2::control_profiles;

    const std::string section = "control_profiles";
    constexpr int num_ports = 4;

    std::mutex mutex;
    Settings defaults;
    std::map<std::string, Settings> store; // By profile name ("" = no profile).
    std::array<std::string, num_ports> port_profiles; // Game thread writes, under the mutex.
    bool loaded = false;
    bool has_edited = false;
    std::string edited; // The Controls tab's picked profile.

    std::string hex_of(const std::string& name) {
        static const char digits[] = "0123456789abcdef";
        std::string out;
        for (unsigned char c : name) {
            out += digits[c >> 4];
            out += digits[c & 15];
        }
        return out;
    }

    std::string name_of_hex(const std::string& hex) {
        std::string out;
        for (size_t i = 0; i + 1 < hex.size(); i += 2) {
            out += (char)std::stoi(hex.substr(i, 2), nullptr, 16);
        }
        return out;
    }

    // Enum values as the settings file (and the old general.json) spells them.
    const char* const steering_keys[] = { "Original", "Balanced", "Linear" };
    const char* const reverse_keys[] = { "Gear", "Hold" };
    const char* const mode_keys[] = { "Automatic", "Original", "Directional" };

    template <typename E, size_t N>
    void read_enum(const nlohmann::json& j, const char* field, const char* const (&keys)[N], E& out) {
        if (!j.contains(field) || !j[field].is_string()) {
            return;
        }
        std::string value = j[field].get<std::string>();
        for (size_t i = 0; i < N; i++) {
            if (value == keys[i]) {
                out = static_cast<E>(i);
            }
        }
    }

    void read_number(const nlohmann::json& j, const char* field, float& out, float min, float max) {
        if (j.contains(field) && j[field].is_number()) {
            out = std::clamp(j[field].get<float>(), min, max);
        }
    }

    Settings from_json(const nlohmann::json& j, Settings s) {
        read_enum(j, "steering", steering_keys, s.steering);
        read_enum(j, "reverse", reverse_keys, s.reverse);
        read_number(j, "deadzone", s.deadzone, 0.0f, 100.0f);
        read_number(j, "rumble_strength", s.rumble_strength, 0.0f, 100.0f);
        read_enum(j, "rumble_mode", mode_keys, s.rumble_mode);
        if (s.rumble_mode == RumbleMode::Directional) {
            s.rumble_mode = RumbleMode::Automatic; // The same since uneven motors stopped playing sides.
        }
        read_number(j, "crash_strength", s.crash_strength, 0.0f, 100.0f);
        read_number(j, "screech_strength", s.screech_strength, 0.0f, 100.0f);
        read_number(j, "grind_strength", s.grind_strength, 0.0f, 100.0f);
        read_number(j, "far_side_cut", s.far_side_cut, 0.0f, 100.0f);
        read_number(j, "screech_chatter", s.screech_chatter, 2.0f, 30.0f);
        read_number(j, "grind_roughness", s.grind_roughness, 0.0f, 100.0f);
        read_number(j, "thump_hz", s.thump_hz, 30.0f, 300.0f);
        read_number(j, "buzz_hz", s.buzz_hz, 30.0f, 300.0f);
        read_number(j, "grind_hz", s.grind_hz, 30.0f, 300.0f);
        return s;
    }

    nlohmann::json to_json(const Settings& s) {
        return {
            { "steering", steering_keys[(int)s.steering] },
            { "reverse", reverse_keys[(int)s.reverse] },
            { "deadzone", s.deadzone },
            { "rumble_strength", s.rumble_strength },
            { "rumble_mode", mode_keys[(int)s.rumble_mode] },
            { "crash_strength", s.crash_strength },
            { "screech_strength", s.screech_strength },
            { "grind_strength", s.grind_strength },
            { "far_side_cut", s.far_side_cut },
            { "screech_chatter", s.screech_chatter },
            { "grind_roughness", s.grind_roughness },
            { "thump_hz", s.thump_hz },
            { "buzz_hz", s.buzz_hz },
            { "grind_hz", s.grind_hz },
        };
    }

    // The old global options in general.json, under their old ids, as the starting defaults.
    Settings migrate_from_general() {
        Settings s;
        std::ifstream file(recomp::get_config_path() / "general.json");
        if (!file.good()) {
            return s;
        }
        nlohmann::json j = nlohmann::json::parse(file, nullptr, false);
        if (j.is_discarded() || !j.is_object()) {
            return s;
        }
        read_enum(j, "steering_response", steering_keys, s.steering);
        read_enum(j, "reverse_control", reverse_keys, s.reverse);
        read_number(j, "joystick_deadzone", s.deadzone, 0.0f, 100.0f);
        read_number(j, "rumble_strength", s.rumble_strength, 0.0f, 100.0f);
        read_enum(j, "rumble_mode", mode_keys, s.rumble_mode);
        if (s.rumble_mode == RumbleMode::Directional) {
            s.rumble_mode = RumbleMode::Automatic;
        }
        return s;
    }

    // Caller holds the mutex.
    void save_locked() {
        nlohmann::json list = nlohmann::json::array();
        for (const auto& [name, settings] : store) {
            list.push_back({ { "name", name }, { "key", hex_of(name) }, { "settings", to_json(settings) } });
        }
        nlohmann::json j{ { "version", 1 }, { "defaults", to_json(defaults) }, { "profiles", list } };
        if (has_edited) {
            j["edited"] = hex_of(edited);
        }
        rush2::data_files::write(rush2::data_files::File::Players, section, j.dump());
    }
}

float Settings::steering_exponent() const {
    // Exponents applied before the game's cube: 1 keeps x^3, 0.55 gives about x^1.65, 1/3 gives x.
    switch (steering) {
        default:
        case SteeringResponse::Original:
            return 1.0f;
        case SteeringResponse::Balanced:
            return 0.55f;
        case SteeringResponse::Linear:
            return 1.0f / 3.0f;
    }
}

void rush2::control_profiles::load() {
    std::lock_guard lock{ mutex };
    loaded = true;
    std::string text = rush2::data_files::read(rush2::data_files::File::Players, section);
    nlohmann::json j = text.empty() ? nlohmann::json() : nlohmann::json::parse(text, nullptr, false);
    if (j.is_discarded() || !j.is_object()) {
        defaults = migrate_from_general();
        store.clear();
        save_locked();
        return;
    }
    defaults = j.contains("defaults") ? from_json(j["defaults"], Settings{}) : Settings{};
    store.clear();
    if (j.contains("profiles") && j["profiles"].is_array()) {
        for (const auto& p : j["profiles"]) {
            if (!p.is_object() || !p.contains("key") || !p["key"].is_string() || !p.contains("settings")) {
                continue;
            }
            store[name_of_hex(p["key"].get<std::string>())] = from_json(p["settings"], defaults);
        }
    }
    has_edited = j.contains("edited") && j["edited"].is_string();
    edited = has_edited ? name_of_hex(j["edited"].get<std::string>()) : std::string();
}

bool rush2::control_profiles::edited_profile(std::string& out) {
    std::lock_guard lock{ mutex };
    out = edited;
    return has_edited;
}

void rush2::control_profiles::set_edited_profile(const std::string& profile) {
    std::lock_guard lock{ mutex };
    if (has_edited && edited == profile) {
        return;
    }
    has_edited = true;
    edited = profile;
    save_locked();
}

Settings rush2::control_profiles::get(const std::string& profile) {
    std::lock_guard lock{ mutex };
    auto it = store.find(profile);
    return it == store.end() ? defaults : it->second;
}

void rush2::control_profiles::set(const std::string& profile, const Settings& settings) {
    std::lock_guard lock{ mutex };
    store[profile] = settings;
    save_locked();
}

SteeringResponse rush2::control_profiles::default_steering() {
    std::lock_guard lock{ mutex };
    return defaults.steering;
}

void rush2::control_profiles::set_default_steering(SteeringResponse steering) {
    std::lock_guard lock{ mutex };
    if (defaults.steering == steering) {
        return;
    }
    defaults.steering = steering;
    save_locked();
}

void rush2::control_profiles::update_ports(uint8_t* rdram) {
    std::array<std::string, num_ports> names;
    for (int port = 0; port < num_ports; port++) {
        int player = rush2::input::port_player(port);
        names[port] = player >= 0 ? rush2::collectibles::player_name(rdram, player) : std::string();
    }
    std::lock_guard lock{ mutex };
    for (int port = 0; port < num_ports; port++) {
        // A port without a player (not joined yet, or between games) keeps the profile it last had.
        if (rush2::input::port_player(port) >= 0) {
            port_profiles[port] = names[port];
        }
    }
}

Settings rush2::control_profiles::port_settings(int port) {
    std::lock_guard lock{ mutex };
    const std::string& name = port >= 0 && port < num_ports ? port_profiles[port] : std::string();
    auto it = store.find(name);
    return it == store.end() ? defaults : it->second;
}

std::string rush2::control_profiles::port_profile(int port) {
    std::lock_guard lock{ mutex };
    return port >= 0 && port < num_ports ? port_profiles[port] : std::string();
}

std::vector<std::string> rush2::control_profiles::profiles() {
    std::vector<std::string> out;
    for (const auto& p : rush2::collectibles::progress()) {
        if (!p.name.empty() && std::find(out.begin(), out.end(), p.name) == out.end()) {
            out.push_back(p.name);
        }
    }
    std::lock_guard lock{ mutex };
    for (const auto& [name, settings] : store) {
        if (!name.empty() && std::find(out.begin(), out.end(), name) == out.end()) {
            out.push_back(name);
        }
    }
    out.push_back("");
    return out;
}
