#include <filesystem>
#include <type_traits>
#include <variant>

#include "recompui/recompui.h"
#include "recompui/config.h"
#include "recompinput/recompinput.h"
#include "ultramodern/config.hpp"
#include "librecomp/config.hpp"
#include "util/file.h"

#include "rush2.h"
#include "track1.h"
#include "wings.h"

// Changes the default of an option the frontend already added. add_option() copies the default into the stored values,
// which is what a fresh install (no saved config file) keeps, so both need changing. Must run before finalize().
template <typename OptionType>
static void set_option_default(recomp::config::Config& config, const std::string& id, recomp::config::ConfigValueVariant value) {
    auto& option = const_cast<recomp::config::ConfigOption&>(config.get_option(id));
    if constexpr (std::is_same_v<OptionType, recomp::config::ConfigOptionNumber>) {
        std::get<OptionType>(option.variant).default_value = std::get<double>(value);
    }
    else {
        std::get<OptionType>(option.variant).default_value = std::get<uint32_t>(value);
    }
    auto& storage = const_cast<recomp::config::ConfigStorage&>(config.get_config_storage());
    storage.value_map[id] = value;
}

// The graphics tab only offers Original/Expand aspect ratios. Rush 2 offers three:
// Original (4:3), 16:9 (RT64's manual aspect, whose default target is 16:9) and Fill (expand to the window).
static void customize_graphics_options(recomp::config::Config& config) {
    namespace options = recompui::config::graphics::options;
    using recomp::config::ConfigOption;
    using recomp::config::ConfigOptionEnum;

    // The config API has no way to replace an option's choices, so edit the option in place before
    // finalize() loads saved values and builds the menu.
    auto& aspect_option = const_cast<ConfigOption&>(config.get_option(options::ar_option));
    aspect_option.description =
        "Sets the horizontal aspect ratio. "
        "<recomp-color primary>Original</recomp-color> uses the game's original 4:3 aspect ratio. "
        "<recomp-color primary>16:9</recomp-color> widens the view to 16:9 and pillarboxes wider windows. "
        "<recomp-color primary>Fill</recomp-color> widens the view to match the game window's aspect ratio.";
    auto& aspect_enum = std::get<ConfigOptionEnum>(aspect_option.variant);
    aspect_enum.options = {
        { ultramodern::renderer::AspectRatio::Original, "Original", "Original (4:3)" },
        { ultramodern::renderer::AspectRatio::Manual, "Manual", "16:9" },
        { ultramodern::renderer::AspectRatio::Expand, "Expand", "Fill" },
    };
    set_option_default<ConfigOptionEnum>(config, options::ar_option, static_cast<uint32_t>(ultramodern::renderer::AspectRatio::Expand));

    // HUD placement moves the race HUD's left and right elements toward the screen edges (src/hud.cpp).
    // The option keys stay the frontend's so saved settings carry over.
    auto& hud_option = const_cast<ConfigOption&>(config.get_option(options::hr_option));
    hud_option.description =
        "Sets how far the race HUD spreads out on wide screens. "
        "<recomp-color primary>Original</recomp-color> keeps it in the centered 4:3 area. "
        "<recomp-color primary>16:9</recomp-color> moves the left and right elements to the edges of a 16:9 area. "
        "<recomp-color primary>Edge</recomp-color> moves them to the edges of the game window.";
    auto& hud_enum = std::get<ConfigOptionEnum>(hud_option.variant);
    hud_enum.options = {
        { ultramodern::renderer::HUDRatioMode::Original, "Original", "Original (4:3)" },
        { ultramodern::renderer::HUDRatioMode::Clamp16x9, "Clamp16x9", "16:9" },
        { ultramodern::renderer::HUDRatioMode::Full, "Expand", "Edge" },
    };
    set_option_default<ConfigOptionEnum>(config, options::hr_option, static_cast<uint32_t>(ultramodern::renderer::HUDRatioMode::Full));

    // Resolution (Auto) and Framerate (Display) already default to what Rush 2 wants.
}

// Rumble defaults to full strength instead of the frontend's 25%.
static void customize_general_options(recomp::config::Config& config) {
    set_option_default<recomp::config::ConfigOptionNumber>(config, recompui::config::general::options::rumble_strength, 100.0);
}

namespace lod_option {
    const std::string id = "lod_mode";
    enum class LODMode : uint32_t { Original, Off };
}

// Level of detail: the game swaps distant models for simpler ones and stops drawing them past a distance (src/lod.cpp).
static void add_lod_option(recomp::config::Config& config) {
    using lod_option::LODMode;

    config.add_enum_option(
        lod_option::id,
        "Level of Detail",
        "Sets whether distant models use simpler versions. "
        "<recomp-color primary>Original</recomp-color> matches the original game. "
        "<recomp-color primary>Off</recomp-color> always draws every model at full detail and stops models from "
        "disappearing in the distance.",
        {
            { LODMode::Original, "Original", "Original" },
            { LODMode::Off, "Off", "Off" },
        },
        LODMode::Off
    );

    // Called when the config loads and on every change, including previews in the menu.
    config.add_option_change_callback(lod_option::id,
        [](recomp::config::ConfigValueVariant cur_value, recomp::config::ConfigValueVariant, recomp::config::OptionChangeContext) {
            rush2::set_lod_disabled(static_cast<LODMode>(std::get<uint32_t>(cur_value)) == LODMode::Off);
        });
}

namespace font_option {
    const std::string id = "font_mode";
    enum class FontMode : uint32_t { Original, HighResolution };
}

// Fonts: swaps the game's text for high-resolution versions from the built-in texture pack (src/fonts.cpp).
static void add_font_option(recomp::config::Config& config) {
    using font_option::FontMode;

    config.add_enum_option(
        font_option::id,
        "Fonts",
        "Sets how the game's text and race HUD numbers are drawn. "
        "<recomp-color primary>Original</recomp-color> uses the original low-resolution fonts. "
        "<recomp-color primary>High Resolution</recomp-color> uses redrawn versions that stay sharp at any resolution.",
        {
            { FontMode::Original, "Original", "Original" },
            { FontMode::HighResolution, "HighResolution", "High Resolution" },
        },
        FontMode::HighResolution
    );

    config.add_option_change_callback(font_option::id,
        [](recomp::config::ConfigValueVariant cur_value, recomp::config::ConfigValueVariant, recomp::config::OptionChangeContext) {
            rush2::set_hires_fonts_enabled(static_cast<FontMode>(std::get<uint32_t>(cur_value)) == FontMode::HighResolution);
        });
}

namespace steering_option {
    const std::string id = "steering_response";
    enum class SteeringResponse : uint32_t { Original, Balanced, Linear };
}

// Steering response: the game cubes the stick's X deflection, so small movements barely steer and the last part of
// the stick's travel does most of the turning (src/input.cpp).
static void add_steering_option(recomp::config::Config& config) {
    using steering_option::SteeringResponse;

    config.add_enum_option(
        steering_option::id,
        "Steering Response",
        "Sets how strongly the car steers for small stick movements. "
        "<recomp-color primary>Original</recomp-color> matches the original game, which barely steers near the "
        "center of the stick and turns sharply near the edge. "
        "<recomp-color primary>Balanced</recomp-color> steers more near the center. "
        "<recomp-color primary>Linear</recomp-color> makes steering proportional to how far the stick is pushed.",
        {
            { SteeringResponse::Original, "Original", "Original" },
            { SteeringResponse::Balanced, "Balanced", "Balanced" },
            { SteeringResponse::Linear, "Linear", "Linear" },
        },
        SteeringResponse::Original
    );

    config.add_option_change_callback(steering_option::id,
        [](recomp::config::ConfigValueVariant cur_value, recomp::config::ConfigValueVariant, recomp::config::OptionChangeContext) {
            // Exponents applied before the game's cube: 1 keeps x^3, 0.55 gives about x^1.65, 1/3 gives x.
            switch (static_cast<SteeringResponse>(std::get<uint32_t>(cur_value))) {
                default:
                case SteeringResponse::Original:
                    rush2::input::set_steering_exponent(1.0f);
                    break;
                case SteeringResponse::Balanced:
                    rush2::input::set_steering_exponent(0.55f);
                    break;
                case SteeringResponse::Linear:
                    rush2::input::set_steering_exponent(1.0f / 3.0f);
                    break;
            }
        });
}

namespace reverse_option {
    const std::string id = "reverse_control";
    enum class ReverseControl : uint32_t { Gear, Hold };
}

// Reverse control: the game's REVERSE button only selects the reverse gear while it's held, so the car needs gas
// too (src/input.cpp).
static void add_reverse_option(recomp::config::Config& config) {
    using reverse_option::ReverseControl;

    config.add_enum_option(
        reverse_option::id,
        "Reverse Control",
        "Sets how the Reverse button works. "
        "<recomp-color primary>Gear</recomp-color> matches the original game: Reverse selects the reverse gear while "
        "it's held, and the car backs up when the gas is pressed too. "
        "<recomp-color primary>Hold</recomp-color> drives the car backward whenever Reverse is held, without needing the gas.",
        {
            { ReverseControl::Gear, "Gear", "Gear" },
            { ReverseControl::Hold, "Hold", "Hold" },
        },
        ReverseControl::Gear
    );

    config.add_option_change_callback(reverse_option::id,
        [](recomp::config::ConfigValueVariant cur_value, recomp::config::ConfigValueVariant, recomp::config::OptionChangeContext) {
            rush2::input::set_reverse_holds_gas(static_cast<ReverseControl>(std::get<uint32_t>(cur_value)) == ReverseControl::Hold);
        });
}

namespace data_location_option {
    const std::string id = "data_location";
    enum class DataLocation : uint32_t { AppData, Portable };
}

// Data location: portable mode stores settings and saves in the program's folder (src/data_location.cpp). The real
// setting is portable.txt, so the saved value is ignored and the option always shows the mode this session started in.
static void add_data_location_option(recomp::config::Config& config) {
    using data_location_option::DataLocation;
    DataLocation current = rush2::data_location::is_portable() ? DataLocation::Portable : DataLocation::AppData;

    config.add_enum_option(
        data_location_option::id,
        "Data Location",
        "Sets where settings, saves and mods are stored. "
        "<recomp-color primary>App Data</recomp-color> uses the user's app data folder. "
        "<recomp-color primary>Portable</recomp-color> uses the game's own folder, so the game can be moved or run "
        "from a USB drive with its data. "
        "Takes effect after restarting the game, which copies the current data to the new location.",
        {
            { DataLocation::AppData, "AppData", "App Data" },
            { DataLocation::Portable, "Portable", "Portable" },
        },
        current
    );

    config.on_json_parse_option(data_location_option::id, [current](const nlohmann::json&) -> recomp::config::ConfigValueVariant {
        return static_cast<uint32_t>(current);
    });

    config.add_option_change_callback(data_location_option::id,
        [](recomp::config::ConfigValueVariant cur_value, recomp::config::ConfigValueVariant, recomp::config::OptionChangeContext context) {
            if (context != recomp::config::OptionChangeContext::Permanent) {
                return;
            }
            if (!rush2::data_location::set_portable(static_cast<DataLocation>(std::get<uint32_t>(cur_value)) == DataLocation::Portable)) {
                recompui::file::show_error_message_box("Data Location",
                    "The data location couldn't be changed because the game's folder isn't writable.");
            }
        });
}

void rush2::init_config() {
    std::filesystem::path recomp_dir = recompui::file::get_app_folder_path();
    if (!recomp_dir.empty()) {
        std::filesystem::create_directories(recomp_dir);
    }

    recompui::config::GeneralTabOptions general_options{};
    general_options.has_rumble_strength = true;
    general_options.has_gyro_sensitivity = false;
    general_options.has_mouse_sensitivity = false;
    auto& general_config = recompui::config::create_general_tab(general_options);
    customize_general_options(general_config);
    add_steering_option(general_config);
    add_reverse_option(general_config);
    add_data_location_option(general_config);

    auto& graphics_config = recompui::config::create_graphics_tab();
    customize_graphics_options(graphics_config);
    add_lod_option(graphics_config);
    add_font_option(graphics_config);

    // The frontend's Controls tab is replaced by the Players tab and the game's own Controller Setup screen.
    rush2::players::create_tab();
    recompui::config::create_sound_tab();
    rush2::cheats::create_tab();
    rush2::wings::create_tab();
    rush2::track1::create_tab();
    recompui::config::create_mods_tab();

    recompui::config::finalize();

    // A fresh install loads without calling option change callbacks, so apply these from the loaded values. The tab
    // references from create_*_tab() may have moved as later tabs were added, so look the config up again.
    auto& loaded_graphics_config = recompui::config::get_graphics_config();
    rush2::set_lod_disabled(static_cast<lod_option::LODMode>(
        std::get<uint32_t>(loaded_graphics_config.get_option_value(lod_option::id))) == lod_option::LODMode::Off);
    rush2::set_hires_fonts_enabled(static_cast<font_option::FontMode>(
        std::get<uint32_t>(loaded_graphics_config.get_option_value(font_option::id))) == font_option::FontMode::HighResolution);
    rush2::input::load_players();
    rush2::controls::load();
    rush2::wings::load_config();
    rush2::track1::load_config();
}
