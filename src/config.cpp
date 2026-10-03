#include <filesystem>
#include <variant>

#include "recompui/recompui.h"
#include "recompui/config.h"
#include "recompinput/recompinput.h"
#include "ultramodern/config.hpp"
#include "librecomp/config.hpp"
#include "util/file.h"

#include "rush2.h"
#include "wings.h"

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
    aspect_enum.default_value = static_cast<uint32_t>(ultramodern::renderer::AspectRatio::Expand);

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
        LODMode::Original
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
    add_steering_option(general_config);

    auto& graphics_config = recompui::config::create_graphics_tab();
    customize_graphics_options(graphics_config);
    add_lod_option(graphics_config);
    add_font_option(graphics_config);

    recompui::config::create_controls_tab();
    recompui::config::create_sound_tab();
    rush2::cheats::create_tab();
    rush2::wings::create_tab();
    recompui::config::create_mods_tab();

    recompui::config::finalize();
    rush2::wings::load_config();
}
