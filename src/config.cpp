#include <filesystem>
#include <variant>

#include "recompui/recompui.h"
#include "recompui/config.h"
#include "recompinput/recompinput.h"
#include "ultramodern/config.hpp"
#include "librecomp/config.hpp"
#include "util/file.h"

#include "rush2.h"

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

    // HUD placement only affects games that use RT64's extended GBI, which Rush 2 doesn't.
    const_cast<ConfigOption&>(config.get_option(options::hr_option)).hidden = true;
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
    recompui::config::create_general_tab(general_options);

    auto& graphics_config = recompui::config::create_graphics_tab();
    customize_graphics_options(graphics_config);

    recompui::config::create_controls_tab();
    recompui::config::create_sound_tab();
    recompui::config::create_mods_tab();

    recompui::config::finalize();
}
