#include <algorithm>
#include <filesystem>
#include <functional>
#include <map>
#include <type_traits>
#include <variant>

#include "recompui/recompui.h"
#include "recompui/config.h"
#include "recompinput/recompinput.h"
#include "ultramodern/config.hpp"
#include "librecomp/config.hpp"
#include "librecomp/game.hpp"
#include "util/file.h"
#include "common/rt64_enhancement_configuration.h"

#include "circuit.h"
#include "npc_cars.h"
#include "rush2.h"
#include "online.h"
#include "players4.h"
#include "music.h"
#include "pause_menu.h"
#include "track1.h"
#include "arcade2049.h"
#include "calspeed.h"
#include "track2049.h"
#include "collectibles.h"
#include "elements/ui_modal.h"
#include "options_page.h"
#include "texture_upscale.h"
#include "wings.h"
#include "ghost.h"
#include "game_filter.h"
#include "car2049.h"
#include "car_probes.h"
#include "control_profiles.h"
#include "setup.h"

// Adds every option of the config that the page doesn't have yet, in the config's order (hidden ones stay hidden).
static void add_remaining_options(rush2::ui::OptionsPage* page, recomp::config::Config& config, const std::vector<std::string>& added) {
    for (const auto& option : config.get_config_schema().options) {
        if (std::find(added.begin(), added.end(), option.id) == added.end()) {
            page->add_option(config, option.id);
        }
    }
}

// A heading of an ordered tab and its option ids. new_column starts it at the top of the next column.
struct TabSection {
    std::string heading;
    std::vector<std::string> ids;
    bool new_column = false;
};

// Shows a config's options under headings in columns (rush2::ui::OptionsPage's column layout), replacing the frontend's
// tab for it (which lists them in the order they were added, and is hidden by the caller). Options not listed go last.
// section_extras adds elements at the end of the named sections.
static void create_ordered_tab(const std::string& name, const std::string& tab_id, const std::string& config_id, int columns,
                               std::vector<TabSection> sections,
                               std::function<void(rush2::ui::OptionsPage*)> add_extra = nullptr,
                               std::map<std::string, std::function<void(rush2::ui::OptionsPage*)>> section_extras = {}) {
    recompui::config::create_tab(name, tab_id,
        [config_id, columns, sections, add_extra, section_extras](recompui::ContextId context, recompui::Element* parent) {
            recomp::config::Config& config = recompui::config::get_config(config_id);
            auto* page = context.create_element<rush2::ui::OptionsPage>(parent, "", columns);
            std::vector<std::string> added;
            for (const auto& [heading, ids, new_column] : sections) {
                if (new_column) {
                    page->next_column();
                }
                if (!heading.empty()) {
                    page->add_heading(heading);
                }
                for (const std::string& id : ids) {
                    page->add_option(config, id);
                    added.push_back(id);
                }
                auto extra = section_extras.find(heading);
                if (extra != section_extras.end()) {
                    extra->second(page);
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

// Rumble strength and the stick deadzone are per profile in the Controls tab (src/controls_tab.cpp). The frontend's
// options stay registered, hidden (its rumble code checks for rumble_strength, and the menus' stick still uses its
// deadzone); their old values became the profiles' starting settings (src/control_profiles.cpp).
static void customize_general_options(recomp::config::Config& config) {
    set_option_default<recomp::config::ConfigOptionNumber>(config, recompui::config::general::options::rumble_strength, 100.0);
    for (const std::string& id : { recompui::config::general::options::rumble_strength,
                                   recompui::config::general::options::joystick_deadzone }) {
        const_cast<recomp::config::ConfigOption&>(config.get_option(id)).hidden = true;
    }
    // Short enough for one line in the General tab's column.
    const_cast<recomp::config::ConfigOption&>(config.get_option(recompui::config::general::options::background_input_mode)).name =
        "Background Input";
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
        true    // Shown in the Controls tab (src/controls_tab.cpp).
    );

    config.add_option_change_callback(split_option::id,
        [](recomp::config::ConfigValueVariant cur_value, recomp::config::ConfigValueVariant, recomp::config::OptionChangeContext) {
            rush2::splitscreen::set_layout(static_cast<Layout>(std::get<uint32_t>(cur_value)));
        });
}

rush2::splitscreen::Layout rush2::splitscreen::get_layout_option() {
    return static_cast<Layout>(std::get<uint32_t>(recompui::config::get_graphics_config().get_option_value(split_option::id)));
}

// The option lives in the graphics settings, which apply changes as they're made; saving writes them to disk.
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
        "<recomp-color primary>HD</recomp-color> uses redrawn versions that stay sharp at any resolution.",
        {
            { FontMode::Original, "Original", "Original" },
            { FontMode::HighResolution, "HighResolution", "HD" },
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

namespace ambient_occlusion_option {
    const std::string id = "ambient_occlusion";
    enum class AmbientOcclusion : uint32_t { Off, Low, Medium, High };

    float strength(recomp::config::ConfigValueVariant value) {
        static const float strengths[] = { 0.0f, 0.35f, 0.6f, 0.85f };
        return strengths[std::min(std::get<uint32_t>(value), 3u)];
    }
}

// Ambient occlusion: darkens the 3D scene's corners and creases from the depth buffer (screen space ambient
// occlusion). A change to RT64, kept in lib/patches/rt64.patch (AmbientOcclusionPS.hlsl, docs/graphics_effects.md).
static void add_ambient_occlusion_option(recomp::config::Config& config) {
    using ambient_occlusion_option::AmbientOcclusion;

    config.add_enum_option(
        ambient_occlusion_option::id,
        "Ambient Occlusion",
        "Shades corners, creases and the ground under walls and barriers, the way less light reaches them, which "
        "gives the scenery more depth. Higher settings shade more. <recomp-color primary>Off</recomp-color> matches "
        "the original game.",
        {
            { AmbientOcclusion::Off, "Off", "Off" },
            { AmbientOcclusion::Low, "Low", "Low" },
            { AmbientOcclusion::Medium, "Medium", "Medium" },
            { AmbientOcclusion::High, "High", "High" },
        },
        AmbientOcclusion::Off
    );

    config.add_option_change_callback(ambient_occlusion_option::id,
        [](recomp::config::ConfigValueVariant cur_value, recomp::config::ConfigValueVariant, recomp::config::OptionChangeContext) {
            RT64::AmbientOcclusionStrength = ambient_occlusion_option::strength(cur_value);
        });
}

namespace car_lighting_option {
    const std::string id = "car_lighting";
}

// Car lighting: the cars take the light of the track around them from light probes baked from the track's own
// colors (src/car_probes.cpp, docs/graphics_effects.md#car-light-probes). Decided per draw, so it switches mid-race.
static void add_car_lighting_option(recomp::config::Config& config) {
    using rush2::probes::Mode;

    config.add_enum_option(
        car_lighting_option::id,
        "Car Lighting",
        "Sets how the track's light falls on the cars. <recomp-color primary>Original</recomp-color> matches the "
        "original games. <recomp-color primary>Light Probes (Experimental)</recomp-color> lights each side of a car by what is around "
        "it: cars darken under bridges and in tunnels, pick up the color of nearby walls and lights, and brighten "
        "toward open sky. Each track is prepared once, the first time it is raced.",
        {
            { Mode::Original, "Original", "Original" },
            { Mode::Probes, "LightProbes", "Light Probes (Experimental)" },
        },
        Mode::Original
    );

    config.add_option_change_callback(car_lighting_option::id,
        [](recomp::config::ConfigValueVariant cur_value, recomp::config::ConfigValueVariant, recomp::config::OptionChangeContext) {
            rush2::probes::set_mode(static_cast<Mode>(std::get<uint32_t>(cur_value)));
        });

    // Disabled for now: the option stays registered so a saved choice is kept, but it is hidden and the cars always
    // use Original lighting (the saved value isn't applied at startup).
    const_cast<recomp::config::ConfigOption&>(config.get_option(car_lighting_option::id)).hidden = true;
}

namespace car_reflections_option {
    const std::string id = "car_reflections";

    // Applies the strength, 5 to 100: RT64 blends the reflections by it.
    void apply(recomp::config::ConfigValueVariant value) {
        RT64::CarReflectionStrength = std::clamp((float)std::get<double>(value) / 100.0f, 0.0f, 1.0f);
    }
}

namespace reflection_rate_option {
    const std::string id = "car_reflection_rate";
    enum class Rate : uint32_t { Thirty, Sixty, EveryFrame };

    // Applies the rate: how many times a second RT64 redraws the reflections, 0 for every frame.
    void apply(recomp::config::ConfigValueVariant value) {
        static const float rates[] = { 30.0f, 60.0f, 0.0f };
        RT64::CarReflectionUpdatesPerSecond = rates[std::min(std::get<uint32_t>(value), 2u)];
    }
}

namespace reflection_blur_option {
    const std::string id = "car_reflection_blur";

    // Applies the blur, 0 to 100 (RT64's CarReflectionPS.hlsl).
    void apply(recomp::config::ConfigValueVariant value) {
        RT64::CarReflectionBlur = std::clamp((float)std::get<double>(value) / 100.0f, 0.0f, 1.0f);
    }
}

namespace car_sheen_option {
    const std::string id = "car_sheen";

    // Applies the mode. The reflection options only do something under Cube Map, so they are grayed out otherwise.
    void apply(recomp::config::Config& config, recomp::config::ConfigValueVariant value) {
        auto mode = static_cast<rush2::car2049::Sheen>(std::get<uint32_t>(value));
        rush2::car2049::set_sheen(mode);
        bool off = mode != rush2::car2049::Sheen::CubeMap;
        config.update_option_disabled(car_reflections_option::id, off);
        config.update_option_disabled(reflection_blur_option::id, off);
        config.update_option_disabled(reflection_rate_option::id, off);
    }
}

// Car sheen: the reflection passes drawn over the cars (src/rush2049/car2049.cpp). Rush 2's cars have sheen layers,
// which All On also makes for the Rush 2049 cars. Under Cube Map the layers draw as the paint reflecting the scene
// (sheen_model): RT64 draws a cube map of the scene around the nearest car
// (docs/rush2049_research/cars.md#car-reflections).
static void add_car_sheen_option(recomp::config::Config& config) {
    using rush2::car2049::Sheen;

    config.add_enum_option(
        car_sheen_option::id,
        "Car Sheen",
        "Sets the reflections on the cars' paint. "
        "<recomp-color primary>Original</recomp-color> matches the original game: a sheen slides over the Rush 2 "
        "cars only. <recomp-color primary>All Off</recomp-color> takes it off every car. "
        "<recomp-color primary>All On</recomp-color> also gives it to the Rush 2049 cars. "
        "<recomp-color primary>Cube Map</recomp-color> makes every car's paint reflect the track, sky and cars "
        "around it during a race, which takes more graphics card time; the three settings below adjust it.",
        {
            { Sheen::Original, "Original", "Original" },
            { Sheen::AllOff, "AllOff", "All Off" },
            { Sheen::AllOn, "AllOn", "All On" },
            { Sheen::CubeMap, "CubeMap", "Cube Map" },
        },
        Sheen::Original
    );

    config.add_option_change_callback(car_sheen_option::id,
        [](recomp::config::ConfigValueVariant cur_value, recomp::config::ConfigValueVariant, recomp::config::OptionChangeContext context) {
            // While the config loads, the tab's config isn't in place yet (touching it crashed): init_config applies
            // the loaded mode, and its graying, once it is.
            if (context == recomp::config::OptionChangeContext::Load) {
                rush2::car2049::set_sheen(static_cast<Sheen>(std::get<uint32_t>(cur_value)));
                return;
            }
            car_sheen_option::apply(recompui::config::get_graphics_config(), cur_value);
        });
}

// The Cube Map sheen's settings: how much of the reflection covers the paint, how soft it is and how often it is
// redrawn (RT64's car reflections, docs/graphics_effects.md#car-reflections).
static void add_car_reflections_option(recomp::config::Config& config) {
    config.add_number_option(
        car_reflections_option::id,
        "Car Reflections",
        "Sets how strongly the paint reflects with Car Sheen set to Cube Map. Higher is shinier.",
        5.0, 100.0, 5.0, 0, true, 70.0
    );

    config.add_option_change_callback(car_reflections_option::id,
        [](recomp::config::ConfigValueVariant cur_value, recomp::config::ConfigValueVariant, recomp::config::OptionChangeContext) {
            car_reflections_option::apply(cur_value);
        });

    config.add_number_option(
        reflection_blur_option::id,
        "Reflection Blur",
        "Sets how soft the Cube Map reflections are. <recomp-color primary>0%</recomp-color> is a sharp mirror; "
        "higher is softer, like glossy paint, and hides the cars' flat panels more.",
        0.0, 100.0, 5.0, 0, true, 20.0
    );

    config.add_option_change_callback(reflection_blur_option::id,
        [](recomp::config::ConfigValueVariant cur_value, recomp::config::ConfigValueVariant, recomp::config::OptionChangeContext) {
            reflection_blur_option::apply(cur_value);
        });

    using reflection_rate_option::Rate;
    config.add_enum_option(
        reflection_rate_option::id,
        "Reflection Rate",
        "Sets how often the Cube Map reflections are redrawn. <recomp-color primary>30</recomp-color> and "
        "<recomp-color primary>60</recomp-color> redraw them that many times a second, and "
        "<recomp-color primary>Every Frame</recomp-color> keeps up with the screen. Higher takes more graphics card "
        "time.",
        {
            { Rate::Thirty, "30", "30" },
            { Rate::Sixty, "60", "60" },
            { Rate::EveryFrame, "EveryFrame", "Every Frame" },
        },
        Rate::Thirty
    );

    config.add_option_change_callback(reflection_rate_option::id,
        [](recomp::config::ConfigValueVariant cur_value, recomp::config::ConfigValueVariant, recomp::config::OptionChangeContext) {
            reflection_rate_option::apply(cur_value);
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
// (src/rush2049/car2049.cpp, docs/rush2049_research/car_physics.md section 9).
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

namespace stunt_gear_option {
    const std::string id = "stunt_first_gear";
}

// Respawn gear: respawns put the human cars in 1st gear instead of 2nd (src/game_fixes.cpp).
static void add_stunt_gear_option(recomp::config::Config& config) {
    config.add_bool_option(
        stunt_gear_option::id,
        "Respawn in 1st Gear",
        "A respawned car with a manual gearbox starts in 1st gear instead of 2nd.",
        false
    );

    config.add_option_change_callback(stunt_gear_option::id,
        [](recomp::config::ConfigValueVariant cur_value, recomp::config::ConfigValueVariant, recomp::config::OptionChangeContext) {
            rush2::set_stunt_first_gear(std::get<bool>(cur_value));
        });
}

namespace hide_respawn_option {
    const std::string id = "hide_respawning_cars";
}

// Hide respawning computer cars: hidden through their glide back after a wreck (src/game_fixes.cpp).
static void add_hide_respawn_option(recomp::config::Config& config) {
    config.add_bool_option(
        hide_respawn_option::id,
        "Hide Respawning Cars",
        "After a computer car's wreck burns out, the car vanishes until it reappears blinking on the road. Off, as in "
        "Rush 2, it sits whole in its fire for a second and a half first.",
        true
    );

    config.add_option_change_callback(hide_respawn_option::id,
        [](recomp::config::ConfigValueVariant cur_value, recomp::config::ConfigValueVariant, recomp::config::OptionChangeContext) {
            rush2::set_hide_respawning_cars(std::get<bool>(cur_value));
        });
}

namespace default_steering_option {
    const std::string id = "default_steering";
}

// The Steering Response a game save profile starts with until it's changed in the Controls tab
// (src/control_profiles.cpp). Its default is what the profiles already started with, so an install from before the
// option keeps it.
static void add_default_steering_option(recomp::config::Config& config) {
    using rush2::control_profiles::SteeringResponse;
    config.add_enum_option(
        default_steering_option::id,
        "New Profile Steering",
        "The Steering Response players start with, until they change it in the Controls tab. "
        "<recomp-color primary>Original</recomp-color> matches the original game: barely any steering near the "
        "center of the stick, sharp turning near the edge. "
        "<recomp-color primary>Balanced</recomp-color> steers more near the center. "
        "<recomp-color primary>Linear</recomp-color> steers as far as the stick is pushed.",
        {
            { SteeringResponse::Original, "Original", "Original" },
            { SteeringResponse::Balanced, "Balanced", "Balanced" },
            { SteeringResponse::Linear, "Linear", "Linear" },
        },
        rush2::control_profiles::default_steering()
    );

    config.add_option_change_callback(default_steering_option::id,
        [](recomp::config::ConfigValueVariant cur_value, recomp::config::ConfigValueVariant, recomp::config::OptionChangeContext) {
            rush2::control_profiles::set_default_steering(static_cast<SteeringResponse>(std::get<uint32_t>(cur_value)));
        });
}

namespace hood_wheels_option {
    const std::string id = "hood_front_wheels";
}

// Hood View Front Wheels: the open-wheel cars keep their front wheels in the hood camera (src/game_fixes.cpp).
static void add_hood_wheels_option(recomp::config::Config& config) {
    config.add_bool_option(
        hood_wheels_option::id,
        "Hood View Front Wheels",
        "In the hood view, the FORMULA, MOUNTAIN DEW and FORMULA 1 show their front wheels. Off, as in Rush 2, the "
        "hood view hides every car's wheels. Takes effect at once, even mid-race.",
        true
    );

    config.add_option_change_callback(hood_wheels_option::id,
        [](recomp::config::ConfigValueVariant cur_value, recomp::config::ConfigValueVariant, recomp::config::OptionChangeContext) {
            rush2::set_hood_front_wheels(std::get<bool>(cur_value));
        });
}

namespace attract_camera_option {
    const std::string id = "attract_camera";
}

// Chase Camera (id attract_camera, kept so saved settings stay valid): the VIEW button also reaches the attract demo's high chase camera (src/camera.cpp).
static void add_attract_camera_option(recomp::config::Config& config) {
    config.add_bool_option(
        attract_camera_option::id,
        "Chase Camera",
        "Adds the attract demo's high chase camera, far behind and high above the car, to the cameras the VIEW "
        "button goes through, after the two chase cameras. Off, as in Rush 2, VIEW goes through four cameras. Takes "
        "effect at the next VIEW press, even mid-race.",
        false
    );

    config.add_option_change_callback(attract_camera_option::id,
        [](recomp::config::ConfigValueVariant cur_value, recomp::config::ConfigValueVariant, recomp::config::OptionChangeContext) {
            rush2::set_attract_camera(std::get<bool>(cur_value));
        });
}

namespace look_back_option {
    const std::string id = "look_back";
}

// Look Back: the LOOK BACK button turns the camera around while held (src/camera.cpp).
static void add_look_back_option(recomp::config::Config& config) {
    config.add_bool_option(
        look_back_option::id,
        "Look Back",
        "Hold the LOOK BACK button (D-pad left by default) to look behind the car. Off, as in Rush 2, there's no LOOK "
        "BACK button and the Controller Setup screen doesn't list it. Takes effect at once.",
        true
    );

    config.add_option_change_callback(look_back_option::id,
        [](recomp::config::ConfigValueVariant cur_value, recomp::config::ConfigValueVariant, recomp::config::OptionChangeContext) {
            rush2::set_look_back(std::get<bool>(cur_value));
        });
}

namespace catch_up_option {
    const std::string id = "drone_catch_up";
}

// Drone catch-up: the computer cars speed up when behind and ease off when ahead (src/npc_cars.cpp).
static void add_catch_up_option(recomp::config::Config& config) {
    config.add_bool_option(
        catch_up_option::id,
        "Drone Cheating",
        "The computer cars speed up when they fall behind the cars around them and ease off when they pull ahead, as "
        "in Rush 2. Off, their pace comes only from DIFFICULTY and their place in the race, not from how far they "
        "are from the other cars. Takes effect at once, even mid-race.",
        true
    );

    config.add_option_change_callback(catch_up_option::id,
        [](recomp::config::ConfigValueVariant cur_value, recomp::config::ConfigValueVariant, recomp::config::OptionChangeContext) {
            rush2::npc_cars::set_catch_up(std::get<bool>(cur_value));
        });
}

namespace torque_option {
    const std::string id = "torque_rebalance";
}

// Torque rebalance: Rush 2's HIGH torque curve is weaker than STANDARD almost everywhere, so it gains top-end torque
// and LOW loses some (src/rush2049/car2049.cpp).
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

namespace circuit_review_option {
    const std::string id = "accurate_circuit_review";
}

// Accurate circuit review: the victory screen after a circuit shows its own races and places (src/circuit.cpp).
static void add_circuit_review_option(recomp::config::Config& config) {
    config.add_bool_option(
        circuit_review_option::id,
        "Accurate Circuit Review",
        "At the end of a circuit, the review of the tracks shows the courses you raced, in order. Each course shows "
        "a racing car if you placed 1st to 3rd on it and a wrecked car if you placed lower. Off, it shows Rush 2's "
        "usual tour of its tracks. "
        "Takes effect the next time a circuit ends.",
        true
    );

    config.add_option_change_callback(circuit_review_option::id,
        [](recomp::config::ConfigValueVariant cur_value, recomp::config::ConfigValueVariant, recomp::config::OptionChangeContext) {
            rush2::circuit::set_accurate_review(std::get<bool>(cur_value));
        });
}

namespace more_circuit_option {
    const std::string id = "more_circuit_settings";
}

// More circuit settings: DIFFICULTY and DEATHS can change on any circuit race (src/circuit.cpp).
static void add_more_circuit_option(recomp::config::Config& config) {
    config.add_bool_option(
        more_circuit_option::id,
        "More Circuit Settings",
        "Lets you change <recomp-color primary>DIFFICULTY</recomp-color> in a circuit, and change it and "
        "<recomp-color primary>DEATHS</recomp-color> before any race of the circuit, not just the first. "
        "Off, a circuit is always at Rush 2's difficulty and DEATHS can only be set before its first race. "
        "Takes effect at once.",
        false
    );

    config.add_option_change_callback(more_circuit_option::id,
        [](recomp::config::ConfigValueVariant cur_value, recomp::config::ConfigValueVariant, recomp::config::OptionChangeContext) {
            rush2::circuit::set_more_settings(std::get<bool>(cur_value));
        });
}

namespace circuit_fixed_difficulty_option {
    const std::string id = "circuit_fixed_difficulty";
}

// Fixed circuit difficulty: a circuit's difficulty no longer follows the standings (src/circuit.cpp).
static void add_circuit_fixed_difficulty_option(recomp::config::Config& config) {
    config.add_bool_option(
        circuit_fixed_difficulty_option::id,
        "Fixed Circuit Difficulty",
        "Stops a circuit from changing its own difficulty. Rush 2 moves the difficulty one step after every circuit "
        "race: up when you lead the standings, down when you trail. On, it stays where it is, or where you set it "
        "with <recomp-color primary>More Circuit Settings</recomp-color>. "
        "Takes effect at the end of the next circuit race.",
        false
    );

    config.add_option_change_callback(circuit_fixed_difficulty_option::id,
        [](recomp::config::ConfigValueVariant cur_value, recomp::config::ConfigValueVariant, recomp::config::OptionChangeContext) {
            rush2::circuit::set_fixed_difficulty(std::get<bool>(cur_value));
        });
}

namespace pause_extras_option {
    const std::string id = "pause_menu_extras";
}

// Pause menu extras: SKIP TRACK, the song box and the collectibles box (src/pause_menu.cpp).
static void add_pause_extras_option(recomp::config::Config& config) {
    config.add_bool_option(
        pause_extras_option::id,
        "Pause Menu Extras",
        "Adds <recomp-color primary>SKIP TRACK</recomp-color> to the pause menu, which plays another song. The menu "
        "also shows the song playing and, racing alone, the keys, Dew cans or coins you've found on the track. "
        "Off, the pause menu is Rush 2's. Takes effect at once, even while paused.",
        true
    );

    config.add_option_change_callback(pause_extras_option::id,
        [](recomp::config::ConfigValueVariant cur_value, recomp::config::ConfigValueVariant, recomp::config::OptionChangeContext) {
            rush2::pause_menu::set_enabled(std::get<bool>(cur_value));
        });
}

namespace menu_background_option {
    const std::string id = "transparent_menu";
}

// Transparent menu: clears the menu's dark backdrop so the game shows through while settings change
// (lib/RecompFrontend ui_modal.cpp, Modal::set_transparent_background).
static void add_menu_background_option(recomp::config::Config& config) {
    config.add_bool_option(
        menu_background_option::id,
        "Transparent Menu",
        "Removes the dark backdrop behind this menu so you can see the game while you change settings. "
        "Takes effect at once.",
        false
    );

    config.add_option_change_callback(menu_background_option::id,
        [](recomp::config::ConfigValueVariant cur_value, recomp::config::ConfigValueVariant, recomp::config::OptionChangeContext) {
            recompui::Modal::set_transparent_background(std::get<bool>(cur_value));
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
    std::filesystem::path recomp_dir = recomp::get_config_path();
    if (!recomp_dir.empty()) {
        std::filesystem::create_directories(recomp_dir);
    }

    // Tabs, grouped: settings, then players and their progress, then what changes the game. The frontend's General
    // and Graphics tabs are replaced by ones that order their options under headings (added first: the menu opens
    // on the first tab, hidden or not), and its Controls tab by ours (src/controls_tab.cpp) and the game's own
    // Controller Setup screen.
    // A fresh install starts on the setup screen (src/setup.cpp); finalize() writes general.json.
    bool fresh_install = !std::filesystem::exists(recomp_dir / (recompui::config::general::id + ".json"));
    rush2::control_profiles::load(); // Before finalize() saves general.json without the options it moves in from.
    {
        namespace general = recompui::config::general;
        // The third column is left for the AI Opponents and Circuit Courses lists, which open there (one at a time),
        // so showing one moves nothing in the others.
        create_ordered_tab(general::tab_name, "rush2_general", general::id, 3, {
            { "Gameplay", { car_stats_option::id, torque_option::id, stunt_gear_option::id, hide_respawn_option::id,
                            default_steering_option::id, hood_wheels_option::id, attract_camera_option::id, look_back_option::id, catch_up_option::id, pause_extras_option::id } },
            { "System", { general::options::background_input_mode, menu_background_option::id, data_location_option::id } },
            { "Circuits", { circuit_review_option::id, more_circuit_option::id, circuit_fixed_difficulty_option::id }, true },
            { "Other", { ghost_option::id, ghosts_kept_option::id, rush2::game_filter::option_id() } },
            { "", {} },     // Run Setup Again, under the rest without a heading
        }, nullptr, {
            { "", [](rush2::ui::OptionsPage* page) {
                recompui::Element* row = page->add_row(
                    "Opens the screen the game started on the first time: pick Original or Enhanced game rules again "
                    "(now " + rush2::setup::rules_name() + "), and add the other games' ROMs.");
                auto* button = recompui::get_current_context().create_element<recompui::Button>(row, "Run Setup Again",
                                                                                               recompui::ButtonStyle::Secondary);
                button->add_pressed_callback([]() {
                    if (recompui::config::close()) {
                        recompui::queue_ui_task([]() { rush2::setup::open(false); });
                    }
                });
            } },
            { "Circuits", [](rush2::ui::OptionsPage* page) {
                rush2::circuit::add_section(page->get_list(), [page](const std::string& text) { return page->text_shower(text); },
                                            page->expand_panel());
            } },
            { "Other", [](rush2::ui::OptionsPage* page) {
                rush2::npc_cars::add_section(page->get_list(), page->text_shower(rush2::npc_cars::description),
                                             page->expand_panel());
            } },
        });
        namespace graphics = recompui::config::graphics;
        create_ordered_tab(graphics::tab_name, "rush2_graphics", graphics::id, 3, {
            { "Display", { graphics::options::wm_option, graphics::options::res_option, graphics::options::ds_option, graphics::options::ar_option,
                           graphics::options::hr_option, graphics::options::rr_option, graphics::options::rr_manual_value,
                           graphics::options::api_option } },
            { "Quality", { graphics::options::msaa_option, anisotropy_option::id, mipmap_option::id, lod_option::id, draw_distance_option::id, font_option::id }, true },
            { "Effects", { ambient_occlusion_option::id, car_lighting_option::id, car_sheen_option::id, car_reflections_option::id, reflection_blur_option::id, reflection_rate_option::id } },
            { "Textures", { rush2::upscale::dreamcast_option_id, rush2::upscale::interface_option_id,
                            rush2::upscale::car_colors_option_id, rush2::upscale::mode_option_id,
                            rush2::upscale::command_option_id }, true },
        }, rush2::upscale::add_buttons);
    }

    recompui::config::GeneralTabOptions general_options{};
    general_options.has_rumble_strength = true;
    general_options.has_gyro_sensitivity = false;
    general_options.has_mouse_sensitivity = false;
    auto& general_config = recompui::config::create_general_tab(general_options);
    customize_general_options(general_config);
    add_ghost_option(general_config);
    add_ghosts_kept_option(general_config);
    add_car_stats_option(general_config);
    add_torque_option(general_config);
    add_stunt_gear_option(general_config);
    add_hide_respawn_option(general_config);
    add_default_steering_option(general_config);
    add_hood_wheels_option(general_config);
    add_attract_camera_option(general_config);
    add_look_back_option(general_config);
    add_catch_up_option(general_config);
    add_pause_extras_option(general_config);
    rush2::game_filter::add_option(general_config);
    add_circuit_review_option(general_config);
    add_more_circuit_option(general_config);
    add_circuit_fixed_difficulty_option(general_config);
    add_menu_background_option(general_config);
    add_data_location_option(general_config);

    auto& graphics_config = recompui::config::create_graphics_tab();
    customize_graphics_options(graphics_config);
    add_lod_option(graphics_config);
    add_draw_distance_option(graphics_config);
    add_font_option(graphics_config);
    add_mipmap_option(graphics_config);
    add_anisotropy_option(graphics_config);
    add_ambient_occlusion_option(graphics_config);
    add_car_lighting_option(graphics_config);
    add_car_sheen_option(graphics_config);
    add_car_reflections_option(graphics_config);
    add_split_option(graphics_config);
    rush2::upscale::add_options(graphics_config);
    recompui::config::set_tab_visible(recompui::config::general::id, false);
    recompui::config::set_tab_visible(recompui::config::graphics::id, false);

    rush2::music::create_sound_tab();
    rush2::npc_cars::init_config();
    rush2::circuit::init_config();
    rush2::controls_tab::create_tab();
    rush2::collectibles::create_tab();
    rush2::games::create_tab();
    rush2::cheats::create_tab();
    rush2::online_tab::create_tab();
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
    RT64::AmbientOcclusionStrength = ambient_occlusion_option::strength(
        loaded_graphics_config.get_option_value(ambient_occlusion_option::id));
    // Car Lighting is disabled for now (add_car_lighting_option): the cars keep Original lighting whatever is saved.
    rush2::probes::set_mode(rush2::probes::Mode::Original);
    car_sheen_option::apply(loaded_graphics_config, loaded_graphics_config.get_option_value(car_sheen_option::id));
    car_reflections_option::apply(loaded_graphics_config.get_option_value(car_reflections_option::id));
    reflection_blur_option::apply(loaded_graphics_config.get_option_value(reflection_blur_option::id));
    reflection_rate_option::apply(loaded_graphics_config.get_option_value(reflection_rate_option::id));
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
    rush2::set_stunt_first_gear(std::get<bool>(
        recompui::config::get_general_config().get_option_value(stunt_gear_option::id)));
    rush2::set_hide_respawning_cars(std::get<bool>(
        recompui::config::get_general_config().get_option_value(hide_respawn_option::id)));
    rush2::control_profiles::set_default_steering(static_cast<rush2::control_profiles::SteeringResponse>(std::get<uint32_t>(
        recompui::config::get_general_config().get_option_value(default_steering_option::id))));
    rush2::set_hood_front_wheels(std::get<bool>(
        recompui::config::get_general_config().get_option_value(hood_wheels_option::id)));
    rush2::set_attract_camera(std::get<bool>(
        recompui::config::get_general_config().get_option_value(attract_camera_option::id)));
    rush2::set_look_back(std::get<bool>(
        recompui::config::get_general_config().get_option_value(look_back_option::id)));
    rush2::npc_cars::set_catch_up(std::get<bool>(
        recompui::config::get_general_config().get_option_value(catch_up_option::id)));
    rush2::game_filter::apply(std::get<bool>(
        recompui::config::get_general_config().get_option_value(rush2::game_filter::option_id())));
    recompui::Modal::set_transparent_background(std::get<bool>(
        recompui::config::get_general_config().get_option_value(menu_background_option::id)));
    rush2::pause_menu::set_enabled(std::get<bool>(
        recompui::config::get_general_config().get_option_value(pause_extras_option::id)));
    rush2::circuit::set_accurate_review(std::get<bool>(
        recompui::config::get_general_config().get_option_value(circuit_review_option::id)));
    rush2::circuit::set_more_settings(std::get<bool>(
        recompui::config::get_general_config().get_option_value(more_circuit_option::id)));
    rush2::circuit::set_fixed_difficulty(std::get<bool>(
        recompui::config::get_general_config().get_option_value(circuit_fixed_difficulty_option::id)));
    rush2::input::load_players();
    rush2::controls::load();
    rush2::wings::load_config();
    rush2::npc_cars::load_config();
    rush2::circuit::load_config();
    rush2::track1::load_config();
    rush2::calspeed::load_config();
    rush2::arcade2049::load_config();
    rush2::music::load_config();
    rush2::setup::load(fresh_install);
    // Last: the lobby compares the settings of the configs loaded above.
    rush2::online_tab::load_config();
}
