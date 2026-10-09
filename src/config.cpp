#include <algorithm>
#include <filesystem>
#include <functional>
#include <type_traits>
#include <variant>

#include "recompui/recompui.h"
#include "recompui/config.h"
#include "recompinput/recompinput.h"
#include "ultramodern/config.hpp"
#include "librecomp/config.hpp"
#include "util/file.h"
#include "common/rt64_enhancement_configuration.h"

#include "npc_cars.h"
#include "rush2.h"
#include "players4.h"
#include "music.h"
#include "track1.h"
#include "track2049.h"
#include "collectibles.h"
#include "options_page.h"
#include "texture_upscale.h"
#include "wings.h"
#include "ghost.h"
#include "car2049.h"

// Adds every option of the config that the page doesn't have yet, in the config's order (hidden ones stay hidden).
static void add_remaining_options(rush2::ui::OptionsPage* page, recomp::config::Config& config, const std::vector<std::string>& added) {
    for (const auto& option : config.get_config_schema().options) {
        if (std::find(added.begin(), added.end(), option.id) == added.end()) {
            page->add_option(config, option.id);
        }
    }
}

// Shows a config's options under headings, replacing the frontend's tab for it (which lists them in the order they
// were added, and is hidden by the caller). Each section is a heading and its option ids; options not listed go last.
static void create_ordered_tab(const std::string& name, const std::string& tab_id, const std::string& config_id,
                               std::vector<std::pair<std::string, std::vector<std::string>>> sections,
                               std::function<void(rush2::ui::OptionsPage*)> add_extra = nullptr) {
    recompui::config::create_tab(name, tab_id,
        [config_id, sections, add_extra](recompui::ContextId context, recompui::Element* parent) {
            recomp::config::Config& config = recompui::config::get_config(config_id);
            auto* page = context.create_element<rush2::ui::OptionsPage>(parent);
            std::vector<std::string> added;
            for (const auto& [heading, ids] : sections) {
                page->add_heading(heading);
                for (const std::string& id : ids) {
                    page->add_option(config, id);
                    added.push_back(id);
                }
            }
            add_remaining_options(page, config, added);
            if (add_extra) {
                add_extra(page);
            }
        },
        [config_id, name](recompui::TabCloseContext close_context) {
            return rush2::ui::confirm_close(config_id, name, close_context);
        },
        [config_id](recompui::TabCloseContext) {
            recomp::config::Config& config = recompui::config::get_config(config_id);
            if (!config.requires_confirmation) {
                config.save_config();
            }
        });
}

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

namespace draw_distance_option {
    const std::string id = "draw_distance";
    enum class DrawDistance : uint32_t { Original, Double, Triple, Quadruple };

    float factor(recomp::config::ConfigValueVariant value) {
        constexpr float factors[] = { 1.0f, 2.0f, 3.0f, 4.0f };
        uint32_t i = std::get<uint32_t>(value);
        return i < 4 ? factors[i] : 1.0f;
    }
}

// Draw distance: how far the track, its objects and the fog reach (src/draw_distance.cpp).
static void add_draw_distance_option(recomp::config::Config& config) {
    using draw_distance_option::DrawDistance;

    config.add_enum_option(
        draw_distance_option::id,
        "Draw Distance",
        "Sets how far away the track and its objects are drawn, with the fog moved out to match. "
        "<recomp-color primary>Original</recomp-color> matches the original game, which hides parts of the track "
        "out of sight of where the camera is. The others draw the whole track, as far as the given multiple of the "
        "original distance.",
        {
            { DrawDistance::Original, "Original", "Original" },
            { DrawDistance::Double, "Double", "2x" },
            { DrawDistance::Triple, "Triple", "3x" },
            { DrawDistance::Quadruple, "Quadruple", "4x" },
        },
        DrawDistance::Double
    );

    config.add_option_change_callback(draw_distance_option::id,
        [](recomp::config::ConfigValueVariant cur_value, recomp::config::ConfigValueVariant, recomp::config::OptionChangeContext) {
            rush2::set_draw_distance(draw_distance_option::factor(cur_value));
        });
}

namespace split_option {
    const std::string id = "split_screen";
}

// Split screen layout of 2 player races (src/splitscreen.cpp).
static void add_split_option(recomp::config::Config& config) {
    using rush2::splitscreen::Layout;

    config.add_enum_option(
        split_option::id,
        "Split Screen",
        "Sets how the screen is split in 2 player races. "
        "<recomp-color primary>Top and Bottom</recomp-color> matches the original game. "
        "<recomp-color primary>Side by Side</recomp-color> gives each player a half of the screen's width. "
        "Takes effect at the start of the next race.",
        {
            { Layout::TopBottom, "TopBottom", "Top and Bottom" },
            { Layout::SideBySide, "SideBySide", "Side by Side" },
        },
        Layout::TopBottom,
        true    // Shown in the Players tab (src/players_tab.cpp).
    );

    config.add_option_change_callback(split_option::id,
        [](recomp::config::ConfigValueVariant cur_value, recomp::config::ConfigValueVariant, recomp::config::OptionChangeContext) {
            rush2::splitscreen::set_layout(static_cast<Layout>(std::get<uint32_t>(cur_value)));
        });
}

rush2::splitscreen::Layout rush2::splitscreen::get_layout_option() {
    return static_cast<Layout>(std::get<uint32_t>(recompui::config::get_graphics_config().get_option_value(split_option::id)));
}

// The option lives in the graphics settings, which hold changes until they're applied. The Graphics tab can't have
// unapplied changes while the Players tab is open, so saving applies only this one.
void rush2::splitscreen::set_layout_option(Layout layout) {
    recomp::config::Config& config = recompui::config::get_graphics_config();
    config.set_option_value(split_option::id, static_cast<uint32_t>(layout));
    config.save_config();
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

namespace mipmap_option {
    const std::string id = "texture_mipmaps";
    enum class Mipmaps : uint32_t { Original, Smooth };
}

// Distant textures: draws the game's textures with mipmaps the renderer generates for them, and texture pack
// replacements with theirs (PNGs get generated ones when they load). This is a change to RT64,
// kept in lib/patches/rt64.patch (TextureMap::use and sampleTexture in TextureSampler.hlsli; see tools/lib_patch.py).
static void add_mipmap_option(recomp::config::Config& config) {
    using mipmap_option::Mipmaps;

    config.add_enum_option(
        mipmap_option::id,
        "Distant Textures",
        "Sets how textures are drawn far from the camera. "
        "<recomp-color primary>Original</recomp-color> matches the original game, where distant textures shimmer "
        "as the camera moves. "
        "<recomp-color primary>Smooth</recomp-color> blends them down with distance, which removes the shimmer, "
        "and keeps the ones the original game blurs with distance sharper.",
        {
            { Mipmaps::Original, "Original", "Original" },
            { Mipmaps::Smooth, "Smooth", "Smooth" },
        },
        Mipmaps::Smooth
    );

    config.add_option_change_callback(mipmap_option::id,
        [](recomp::config::ConfigValueVariant cur_value, recomp::config::ConfigValueVariant, recomp::config::OptionChangeContext) {
            RT64::GeneratedMipmapsEnabled = static_cast<Mipmaps>(std::get<uint32_t>(cur_value)) == Mipmaps::Smooth;
        });
}

namespace anisotropy_option {
    const std::string id = "texture_anisotropy";
    enum class Anisotropy : uint32_t { Off, X2, X4, X8, X16 }; // The value is the log2 RT64 takes.
}

// Anisotropic filtering of the textures Distant Textures smooths (sampleGeneratedMipmaps in RT64's TextureSampler.hlsli).
static void add_anisotropy_option(recomp::config::Config& config) {
    using anisotropy_option::Anisotropy;

    config.add_enum_option(
        anisotropy_option::id,
        "Anisotropic Filtering",
        "Sets how sharp smoothed textures stay on surfaces seen at a shallow angle, such as the road ahead. "
        "Higher settings are sharper. <recomp-color primary>Off</recomp-color> blurs them the most. "
        "Only applies when Distant Textures is set to <recomp-color primary>Smooth</recomp-color>.",
        {
            { Anisotropy::Off, "Off", "Off" },
            { Anisotropy::X2, "2x", "2x" },
            { Anisotropy::X4, "4x", "4x" },
            { Anisotropy::X8, "8x", "8x" },
            { Anisotropy::X16, "16x", "16x" },
        },
        Anisotropy::X16
    );

    config.add_option_change_callback(anisotropy_option::id,
        [](recomp::config::ConfigValueVariant cur_value, recomp::config::ConfigValueVariant, recomp::config::OptionChangeContext) {
            RT64::GeneratedMipmapsAnisotropy = std::min(std::get<uint32_t>(cur_value), 4u);
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

namespace ghost_option {
    const std::string id = "save_ghosts";
    enum class SaveGhosts : uint32_t { Off, On };
}

// Save ghosts: every race records player 1's car and keeps the profile's fastest finished runs per track as ghosts for
// GHOST RACE (src/ghost.cpp). Rush 2049 only recorded ghosts in practice.
static void add_ghost_option(recomp::config::Config& config) {
    using ghost_option::SaveGhosts;

    config.add_enum_option(
        ghost_option::id,
        "Save Ghosts",
        "Sets which races save ghosts for Ghost Race. "
        "<recomp-color primary>On</recomp-color> keeps player 1's fastest finished runs of each track, direction and "
        "lap count from every race. "
        "<recomp-color primary>Off</recomp-color> only saves them from Ghost Race.",
        {
            { SaveGhosts::Off, "Off", "Off" },
            { SaveGhosts::On, "On", "On" },
        },
        SaveGhosts::On
    );

    config.add_option_change_callback(ghost_option::id,
        [](recomp::config::ConfigValueVariant cur_value, recomp::config::ConfigValueVariant, recomp::config::OptionChangeContext) {
            rush2::ghost::set_save_all(static_cast<SaveGhosts>(std::get<uint32_t>(cur_value)) == SaveGhosts::On);
        });
}

namespace ghosts_kept_option {
    const std::string id = "ghosts_kept";
}

// Ghosts kept: how many of a profile's fastest runs of each track, direction and lap count are kept (src/ghost.cpp).
static void add_ghosts_kept_option(recomp::config::Config& config) {
    config.add_number_option(
        ghosts_kept_option::id,
        "Ghosts Kept",
        "Sets how many of each profile's fastest runs of each track, direction and lap count are kept as ghosts. "
        "A slower run than all of them isn't kept, and lowering this deletes the slowest ones the next time a run "
        "on that track is kept.",
        1.0, 20.0, 1.0, 0, false, 3.0
    );

    config.add_option_change_callback(ghosts_kept_option::id,
        [](recomp::config::ConfigValueVariant cur_value, recomp::config::ConfigValueVariant, recomp::config::OptionChangeContext) {
            rush2::ghost::set_ghosts_kept((int)std::get<double>(cur_value));
        });
}

namespace car_stats_option {
    const std::string id = "accurate_car_stats";
}

// Accurate car stats: the car select's ACCELERATION, TOP SPEED, CONTROL and DRIFTING bars come from the race physics
// (src/car2049.cpp, docs/rush2049_research/car_physics.md section 9).
static void add_car_stats_option(recomp::config::Config& config) {
    config.add_bool_option(
        car_stats_option::id,
        "Accurate Car Stats",
        "Makes the car select's bars measure the car by driving it through the game's own physics, with the chosen "
        "options and Car Speeds. <recomp-color primary>ACCELERATION</recomp-color> is the time from a standstill to "
        "100 mph and <recomp-color primary>TOP SPEED</recomp-color> the top speed. "
        "<recomp-color primary>DRIFTING</recomp-color> is how wide the car slides in one second of full steering at "
        "68 mph. <recomp-color primary>CONTROL</recomp-color> is how quickly the car straightens out of a slide "
        "without fishtailing. Each bar runs from empty at the lowest value any car can reach with any options to "
        "full at the highest. Off, the bars use Rush 2's own formulas.",
        true
    );

    config.add_option_change_callback(car_stats_option::id,
        [](recomp::config::ConfigValueVariant cur_value, recomp::config::ConfigValueVariant, recomp::config::OptionChangeContext) {
            rush2::car2049::set_accurate_bars(std::get<bool>(cur_value));
        });
}

namespace torque_option {
    const std::string id = "torque_rebalance";
}

// Torque rebalance: Rush 2's HIGH torque curve is weaker than STANDARD almost everywhere, so it gains top-end torque
// and LOW loses some (src/car2049.cpp).
static void add_torque_option(recomp::config::Config& config) {
    config.add_bool_option(
        torque_option::id,
        "Torque Rebalance",
        "Makes each TORQUE setting worth picking. In Rush 2, HIGH is slower than STANDARD both off the line and at top "
        "speed, and LOW has the best launch and the best top speed. "
        "On, <recomp-color primary>LOW</recomp-color> accelerates hardest but has the lowest top speed, "
        "<recomp-color primary>HIGH</recomp-color> accelerates slowest but has the highest top speed, and "
        "<recomp-color primary>STANDARD</recomp-color> sits between them on both. "
        "Off, the original curves are used.",
        true
    );

    config.add_option_change_callback(torque_option::id,
        [](recomp::config::ConfigValueVariant cur_value, recomp::config::ConfigValueVariant, recomp::config::OptionChangeContext) {
            rush2::car2049::set_torque_rebalance(std::get<bool>(cur_value));
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

    // Tabs, grouped: settings, then players and their progress, then what changes the game. The frontend's General
    // and Graphics tabs are replaced by ones that order their options under headings (added first: the menu opens
    // on the first tab, hidden or not), and its Controls tab by the Players tab and the game's own Controller Setup
    // screen.
    {
        namespace general = recompui::config::general;
        create_ordered_tab(general::tab_name, "rush2_general", general::id, {
            { "Controls", { general::options::rumble_strength, general::options::joystick_deadzone,
                            steering_option::id, reverse_option::id } },
            { "Gameplay", { car_stats_option::id, torque_option::id, ghost_option::id, ghosts_kept_option::id } },
            { "System", { general::options::background_input_mode, data_location_option::id } },
        });
        namespace graphics = recompui::config::graphics;
        create_ordered_tab(graphics::tab_name, "rush2_graphics", graphics::id, {
            { "Display", { graphics::options::wm_option, graphics::options::res_option, graphics::options::ds_option, graphics::options::ar_option,
                           graphics::options::hr_option, graphics::options::rr_option, graphics::options::rr_manual_value } },
            { "Quality", { graphics::options::msaa_option, anisotropy_option::id, mipmap_option::id, lod_option::id, draw_distance_option::id, font_option::id } },
            { "Texture Upscaling", { rush2::upscale::mode_option_id, rush2::upscale::command_option_id } },
        }, rush2::upscale::add_buttons);
    }

    recompui::config::GeneralTabOptions general_options{};
    general_options.has_rumble_strength = true;
    general_options.has_gyro_sensitivity = false;
    general_options.has_mouse_sensitivity = false;
    auto& general_config = recompui::config::create_general_tab(general_options);
    customize_general_options(general_config);
    add_steering_option(general_config);
    add_reverse_option(general_config);
    add_ghost_option(general_config);
    add_ghosts_kept_option(general_config);
    add_car_stats_option(general_config);
    add_torque_option(general_config);
    add_data_location_option(general_config);

    auto& graphics_config = recompui::config::create_graphics_tab();
    customize_graphics_options(graphics_config);
    add_lod_option(graphics_config);
    add_draw_distance_option(graphics_config);
    add_font_option(graphics_config);
    add_mipmap_option(graphics_config);
    add_anisotropy_option(graphics_config);
    add_split_option(graphics_config);
    rush2::upscale::add_options(graphics_config);
    recompui::config::set_tab_visible(recompui::config::general::id, false);
    recompui::config::set_tab_visible(recompui::config::graphics::id, false);

    rush2::music::create_sound_tab();
    rush2::players::create_tab();
    rush2::collectibles::create_tab();
    rush2::games::create_tab();
    rush2::cheats::create_tab();
    recompui::config::create_mods_tab();

    recompui::config::finalize();

    // A fresh install loads without calling option change callbacks, so apply these from the loaded values. The tab
    // references from create_*_tab() may have moved as later tabs were added, so look the config up again.
    auto& loaded_graphics_config = recompui::config::get_graphics_config();
    // Downsampling Quality only shows at Original resolution (the frontend keeps this up to date on changes).
    loaded_graphics_config.update_option_hidden(recompui::config::graphics::options::ds_option,
        std::get<uint32_t>(loaded_graphics_config.get_option_value(recompui::config::graphics::options::res_option)) !=
        static_cast<uint32_t>(ultramodern::renderer::Resolution::Original));
    rush2::set_lod_disabled(static_cast<lod_option::LODMode>(
        std::get<uint32_t>(loaded_graphics_config.get_option_value(lod_option::id))) == lod_option::LODMode::Off);
    rush2::set_draw_distance(draw_distance_option::factor(
        loaded_graphics_config.get_option_value(draw_distance_option::id)));
    RT64::GeneratedMipmapsEnabled = static_cast<mipmap_option::Mipmaps>(
        std::get<uint32_t>(loaded_graphics_config.get_option_value(mipmap_option::id))) == mipmap_option::Mipmaps::Smooth;
    RT64::GeneratedMipmapsAnisotropy = std::min(
        std::get<uint32_t>(loaded_graphics_config.get_option_value(anisotropy_option::id)), 4u);
    rush2::set_hires_fonts_enabled(static_cast<font_option::FontMode>(
        std::get<uint32_t>(loaded_graphics_config.get_option_value(font_option::id))) == font_option::FontMode::HighResolution);
    rush2::upscale::apply_loaded_options(loaded_graphics_config);
    rush2::track1::add_banner_images();
    rush2::track2049::add_banner_images();
    rush2::splitscreen::set_layout(static_cast<rush2::splitscreen::Layout>(
        std::get<uint32_t>(loaded_graphics_config.get_option_value(split_option::id))));
    rush2::ghost::set_save_all(static_cast<ghost_option::SaveGhosts>(std::get<uint32_t>(
        recompui::config::get_general_config().get_option_value(ghost_option::id))) == ghost_option::SaveGhosts::On);
    rush2::ghost::set_ghosts_kept((int)std::get<double>(
        recompui::config::get_general_config().get_option_value(ghosts_kept_option::id)));
    rush2::car2049::set_accurate_bars(std::get<bool>(
        recompui::config::get_general_config().get_option_value(car_stats_option::id)));
    rush2::car2049::set_torque_rebalance(std::get<bool>(
        recompui::config::get_general_config().get_option_value(torque_option::id)));
    rush2::input::load_players();
    rush2::controls::load();
    rush2::wings::load_config();
    rush2::npc_cars::load_config();
    rush2::track1::load_config();
    rush2::music::load_config();
}
