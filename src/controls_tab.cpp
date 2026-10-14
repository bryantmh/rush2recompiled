// Controls tab: which controllers and the keyboard may play (src/input.cpp), the 2 player split screen layout
// (src/splitscreen.cpp), and below a profile picker each game save profile's steering, stick and rumble settings
// (src/control_profiles.cpp), with buttons that play each rumble effect.
//
// Controllers aren't given to players here: in the game, whoever presses START becomes the next player. Each
// controller (connected, or disabled and remembered) and the keyboard has a toggle that enables it. The controller list
// rebuilds itself when controllers connect or disconnect, or a device's port or player changes.
//
// The profile settings are rows of a config that is never saved as a file (profile_config): picking a profile copies
// its settings into the rows, and changing a row saves the profile through rush2::control_profiles::set. The game
// reads them every frame, so changes apply at once, mid-race too.
//
// Bindings are edited in the game's own Controller Setup screen (src/controls_menu.cpp), which replaces the frontend's
// Controls tab.

#include <algorithm>
#include <string>
#include <vector>

#include "recompui/config.h"
#include "elements/ui_button.h"
#include "elements/ui_label.h"
#include "elements/ui_radio.h"
#include "elements/ui_select.h"
#include "elements/ui_toggle.h"

#include "control_profiles.h"
#include "options_page.h"
#include "rush2.h"
#include "online.h"

namespace {
    using namespace recompui;
    using namespace rush2::control_profiles;

    const std::string tab_id = "rush2_controls";
    const std::string tab_name = "Controls";
    const std::string no_profile_label = "No profile";

    const std::string description =
        "Choose which controllers can play. All of them can unless turned off here.<br/><br/>"
        "Players join in the game: player 1 presses START on the title screen, and players 2, 3 and 4 press START on "
        "Select Player.<br/><br/>"
        "Buttons are set in the game's Controller Setup screen (Options, or the pause menu during a race).<br/><br/>"
        "Steering and rumble settings belong to a game save profile: each player gets the settings of the profile they "
        "picked on Select Player. Changes take effect right away.";

    namespace ids {
        const std::string steering = "steering_response";
        const std::string reverse = "reverse_control";
        const std::string deadzone = "deadzone";
        const std::string rumble_strength = "rumble_strength";
        const std::string rumble_mode = "rumble_mode";
        const std::string crash = "crash_strength";
        const std::string road = "road_strength";
    }

    // Road Rumble scales tire screech and wall grinding together: at 100% they're the defaults below (their strengths
    // next to a crash's, tuned by feel). The finer settings (control_profiles.h: side separation, chatter, roughness,
    // the DualSense pitches) stay at their defaults.
    constexpr float screech_at_full_road = 70.0f;
    constexpr float grind_at_full_road = 85.0f;

    recomp::config::Config profile_config{ "Controls", "control_profile", false };
    std::string selected;      // UI thread: the profile being edited.
    bool picked = false;       // The profile was chosen in the picker; until then it's the first profile.
    bool loading = false;      // The rows are being set from a profile, not changed by the player.

    double number(const std::string& id) {
        return std::get<double>(profile_config.get_option_value(id));
    }

    template <typename E>
    E enum_value(const std::string& id) {
        return static_cast<E>(std::get<uint32_t>(profile_config.get_option_value(id)));
    }

    Settings settings_from_rows() {
        Settings s = get(selected); // The settings without rows keep their saved values.
        s.steering = enum_value<SteeringResponse>(ids::steering);
        s.reverse = enum_value<ReverseControl>(ids::reverse);
        s.deadzone = (float)number(ids::deadzone);
        s.rumble_strength = (float)number(ids::rumble_strength);
        s.rumble_mode = enum_value<RumbleMode>(ids::rumble_mode);
        s.crash_strength = (float)number(ids::crash);
        float road = (float)number(ids::road) / 100.0f;
        s.screech_strength = screech_at_full_road * road;
        s.grind_strength = grind_at_full_road * road;
        return s;
    }

    // Grays out the rumble rows that do nothing with the chosen mode: Original plays the game's one level for
    // everything (no crash or road strength).
    void update_disabled() {
        bool original = enum_value<RumbleMode>(ids::rumble_mode) == RumbleMode::Original;
        profile_config.update_option_disabled(ids::crash, original);
        profile_config.update_option_disabled(ids::road, original);
    }

    void show_profile(const std::string& profile) {
        Settings s = get(profile);
        loading = true;
        profile_config.update_option_value(ids::steering, (uint32_t)s.steering);
        profile_config.update_option_value(ids::reverse, (uint32_t)s.reverse);
        profile_config.update_option_value(ids::deadzone, (double)s.deadzone);
        profile_config.update_option_value(ids::rumble_strength, (double)s.rumble_strength);
        profile_config.update_option_value(ids::rumble_mode, (uint32_t)s.rumble_mode);
        profile_config.update_option_value(ids::crash, (double)s.crash_strength);
        profile_config.update_option_value(ids::road, std::min(100.0, 100.0 * s.screech_strength / screech_at_full_road));
        loading = false;
    }

    void add_profile_options() {
        auto& c = profile_config;
        c.add_enum_option(ids::steering, "Steering Response",
            "How strongly the car steers for small stick movements. "
            "<recomp-color primary>Original</recomp-color> matches the original game: barely any steering near the "
            "center of the stick, sharp turning near the edge. "
            "<recomp-color primary>Balanced</recomp-color> steers more near the center. "
            "<recomp-color primary>Linear</recomp-color> steers as far as the stick is pushed.",
            {
                { SteeringResponse::Original, "Original", "Original" },
                { SteeringResponse::Balanced, "Balanced", "Balanced" },
                { SteeringResponse::Linear, "Linear", "Linear" },
            },
            SteeringResponse::Original);
        c.add_enum_option(ids::reverse, "Reverse Control",
            "How the Reverse button works. "
            "<recomp-color primary>Gear</recomp-color> matches the original game: holding Reverse picks the reverse "
            "gear, and the gas drives backward. "
            "<recomp-color primary>Hold</recomp-color> drives backward whenever Reverse is held, no gas needed.",
            {
                { ReverseControl::Gear, "Gear", "Gear" },
                { ReverseControl::Hold, "Hold", "Hold" },
            },
            ReverseControl::Gear);
        c.add_number_option(ids::deadzone, "Stick Deadzone",
            "How far the stick can move from the center before the car steers. Raise it if the car drifts to one side "
            "with the stick let go.",
            0, 50, 1, 0, true, 5);

        c.add_enum_option(ids::rumble_mode, "Rumble Mode",
            "<recomp-color primary>Enhanced</recomp-color> makes crashes a short heavy thump, scraping along a wall a "
            "rough grind and screeching tires a lighter buzz. Controllers with matching left and right motors also "
            "rumble the side of the car that was hit: a DualSense, a Steam Controller, a Steam Deck, and Xbox "
            "controllers with trigger rumble (through the triggers).<br/><br/>"
            "A DualSense needs a USB cable for its full haptics; over Bluetooth it rumbles like other controllers. "
            "<recomp-color primary>Original</recomp-color> rumbles like the original game.",
            {
                { RumbleMode::Automatic, "Automatic", "Enhanced" },
                { RumbleMode::Original, "Original", "Original" },
            },
            RumbleMode::Automatic);
        c.add_percent_number_option(ids::rumble_strength, "Rumble Strength",
            "How strong all rumble is. 0% turns it off.", 100);
        c.add_percent_number_option(ids::crash, "Crash Rumble",
            "How hard the controller thumps when the car crashes into something.", 100);
        c.add_percent_number_option(ids::road, "Road Rumble",
            "How strongly the controller rumbles for screeching tires and scraping along walls.", 100);

        for (const auto& option : c.get_config_schema().options) {
            c.add_option_change_callback(option.id,
                [](recomp::config::ConfigValueVariant, recomp::config::ConfigValueVariant, recomp::config::OptionChangeContext) {
                    update_disabled();
                    if (!loading) {
                        set(selected, settings_from_rows());
                    }
                });
        }
        update_disabled();
    }

    std::string label_of(const std::string& profile) {
        return profile.empty() ? no_profile_label : profile;
    }

    class ControlsPage : public rush2::ui::OptionsPage {
    public:
        ControlsPage(ResourceId rid, Element* parent) : OptionsPage(rid, parent, description) {
            ContextId context = get_current_context();

            add_heading("Controllers");
            controllers_box = context.create_element<Element>(get_list(), 0, "div", false);
            controllers_box->set_display(Display::Flex);
            controllers_box->set_flex_direction(FlexDirection::Column);
            controllers_box->set_padding_left(12.0f);
            controllers_box->set_padding_right(12.0f);
            controllers_box->set_padding_top(4.0f);
            controllers_box->set_padding_bottom(8.0f);
            controllers_box->set_gap(12.0f);
            controllers_box->set_as_navigation_container(NavigationType::Vertical);
            render_controllers();

            add_heading("Split Screen", "", "How the screen is split in 2 player races. "
                "<recomp-color primary>Top and Bottom</recomp-color> matches the original game; "
                "<recomp-color primary>Side by Side</recomp-color> gives each player half of the screen's width. "
                "Takes effect at the next race.");
            using rush2::splitscreen::Layout;
            Element* split_row = add_row("How the screen is split in 2 player races. Takes effect at the next race.");
            Radio* layouts = context.create_element<Radio>(split_row);
            layouts->add_option("Top and Bottom");
            layouts->add_option("Side by Side");
            layouts->set_index(rush2::splitscreen::get_layout_option() == Layout::SideBySide ? 1 : 0);
            layouts->add_index_changed_callback([](uint32_t index) {
                rush2::splitscreen::set_layout_option(index == 1 ? Layout::SideBySide : Layout::TopBottom);
            });

            const std::string profile_description = "The game save profile whose settings are shown below. Each player "
                "uses the settings of the profile they picked on Select Player; " + no_profile_label + " is for players "
                "without one.";
            add_heading("Profile", "", profile_description);
            picker = add_row(profile_description);
            refresh_profiles(true);

            add_heading("Driving");
            add_option(profile_config, ids::steering);

            add_heading("Online", "", "How other players see you in online races (the Online tab).");
            add_option(rush2::online_tab::config(), "display_name");
            add_option(profile_config, ids::reverse);
            add_option(profile_config, ids::deadzone);

            add_heading("Rumble");
            add_option(profile_config, ids::rumble_mode);
            add_option(profile_config, ids::rumble_strength);
            add_option(profile_config, ids::crash);
            add_option(profile_config, ids::road);
            Element* tests = add_row("Plays an effect on the controller you're holding with this profile's settings, "
                "as in a race. Screech and Grind play on the left side, then the right.");
            add_test(tests, "Left Crash", rush2::input::RumbleTest::LeftCrash);
            add_test(tests, "Right Crash", rush2::input::RumbleTest::RightCrash);
            add_test(tests, "Screech", rush2::input::RumbleTest::Screech);
            add_test(tests, "Grind", rush2::input::RumbleTest::Grind);
        }

    protected:
        std::string_view get_type_name() override { return "Rush2ControlsPage"; }

        void on_update() override {
            if (controllers_state() != rendered_state) {
                // The rebuilt toggle of the one that had focus takes it back.
                int refocus = -1;
                for (size_t i = 0; i < toggles.size(); i++) {
                    if (toggles[i]->is_style_enabled(focus_state)) {
                        refocus = (int)i;
                    }
                }
                render_controllers();
                if (refocus >= 0 && refocus < (int)toggles.size()) {
                    toggles[refocus]->focus();
                }
            }
            // The profiles change as players make them in the game; checked twice a second.
            if (++frames % 30 == 0) {
                refresh_profiles(false);
            }
        }

    private:
        Element* controllers_box = nullptr;
        Element* picker = nullptr;
        std::string rendered_state;
        std::string shown_profiles;
        std::vector<Toggle*> toggles;
        int frames = 0;

        void add_test(Element* row, const std::string& name, rush2::input::RumbleTest test) {
            Button* button = get_current_context().create_element<Button>(row, name, ButtonStyle::Secondary);
            button->set_flex_shrink(0.0f);
            button->add_pressed_callback([test]() {
                rush2::input::test_rumble(test, settings_from_rows());
            });
        }

        // Rebuilds the picker when the profiles change (rebuilding it would drop its focus), and shows the selected
        // profile's settings when it changes.
        void refresh_profiles(bool force) {
            std::vector<std::string> list = profiles();
            std::string names;
            bool found = false;
            for (const std::string& p : list) {
                names += label_of(p) + ";";
                found = found || p == selected;
            }
            std::string before = selected;
            if (!found || !picked) {
                selected = list.front(); // The first profile (No profile only when there are none).
                picked = picked && found;
            }
            if (force || selected != before) {
                show_profile(selected);
            }
            if (!force && names == shown_profiles && selected == before) {
                return;
            }
            shown_profiles = names;
            ContextId context = get_current_context();
            picker->clear_children();
            std::vector<SelectOption> options;
            for (size_t i = 0; i < list.size(); i++) {
                options.emplace_back(label_of(list[i]), std::to_string(i));
            }
            size_t index = 0;
            for (size_t i = 0; i < list.size(); i++) {
                if (list[i] == selected) {
                    index = i;
                }
            }
            Select* select = context.create_element<Select>(picker, options, std::to_string(index));
            // Select is focusable but not in the tab order, which the d-pad's navigation (RmlUi's nav: auto) needs.
            select->set_tab_index_auto();
            select->add_change_callback([list](SelectOption& option, int) {
                size_t i = (size_t)std::stoi(option.value);
                if (i < list.size()) {
                    selected = list[i];
                    picked = true;
                    show_profile(selected);
                }
            });
        }

        // Everything the controller list shows, so it's only rebuilt when it changes.
        static std::string controllers_state() {
            std::string state;
            for (const auto& c : rush2::input::get_controllers()) {
                state += std::to_string(c.joystick_id) + c.key + ";";
            }
            for (const auto& c : rush2::input::get_disabled_controllers()) {
                state += c.key + ",";
            }
            for (int port = 0; port < rush2::input::num_ports; port++) {
                state += "|" + std::to_string(rush2::input::get_port_controller(port)) + ":" +
                    std::to_string(rush2::input::port_player(port));
            }
            return state + "|" + std::to_string(rush2::input::get_keyboard_port()) + "|" +
                std::to_string(rush2::input::get_keyboard_enabled());
        }

        // What a device is doing right now, from its port (-1 for none).
        static std::string port_status(int port) {
            if (port < 0) {
                return "Press a button on it to use it";
            }
            int player = rush2::input::port_player(port);
            if (player >= 0) {
                std::string profile = port_profile(port);
                return "Player " + std::to_string(player + 1) + (profile.empty() ? "" : " (" + profile + ")");
            }
            return "Ready: press START in the game to join";
        }

        // A line of the list: a name and status on the left and a toggle on the right.
        Toggle* add_toggle_line(const std::string& name, const std::string& status, bool checked) {
            ContextId context = get_current_context();
            Element* line = context.create_element<rush2::ui::FocusRow>(controllers_box);
            line->set_display(Display::Flex);
            line->set_flex_direction(FlexDirection::Row);
            line->set_align_items(AlignItems::Center);
            line->set_gap(16.0f);
            line->set_as_navigation_container(NavigationType::Horizontal);
            Element* text = context.create_element<Element>(line, 0, "div", false);
            text->set_display(Display::Flex);
            text->set_flex_direction(FlexDirection::Column);
            text->set_flex_grow(1.0f);
            text->set_gap(4.0f);
            context.create_element<Label>(text, name, theme::Typography::LabelSM);
            Label* status_label = context.create_element<Label>(text, status, theme::Typography::Body);
            status_label->set_color(theme::color::TextDim);
            Toggle* toggle = context.create_element<Toggle>(line, ToggleSize::Medium);
            toggle->set_checked(checked);
            toggles.push_back(toggle);
            return toggle;
        }

        void render_controllers() {
            ContextId context = get_current_context();
            rendered_state = controllers_state();
            controllers_box->clear_children();
            toggles.clear();

            // The connected controllers, then the disabled ones that aren't connected.
            std::vector<rush2::input::ControllerInfo> controllers = rush2::input::get_controllers();
            std::vector<rush2::input::DisabledController> disabled = rush2::input::get_disabled_controllers();
            if (controllers.empty() && disabled.empty()) {
                Label* none = context.create_element<Label>(controllers_box, "No game controllers connected", theme::Typography::Body);
                none->set_color(theme::color::TextDim);
            }
            for (size_t i = 0; i < controllers.size(); i++) {
                // Number identical controllers so they can be told apart.
                std::string name = controllers[i].name;
                int same = 0, index = 0;
                for (size_t j = 0; j < controllers.size(); j++) {
                    if (controllers[j].name == controllers[i].name) {
                        same++;
                        index += j < i;
                    }
                }
                if (same > 1) {
                    name += " (" + std::to_string(index + 1) + ")";
                }
                bool enabled = rush2::input::is_controller_enabled(controllers[i].joystick_id);
                int port = -1;
                for (int p = 0; p < rush2::input::num_ports; p++) {
                    if (rush2::input::get_port_controller(p) == controllers[i].joystick_id) {
                        port = p;
                    }
                }
                Toggle* toggle = add_toggle_line(name, enabled ? port_status(port) : "Off", enabled);
                std::string key = controllers[i].key;
                std::string base_name = controllers[i].name;
                toggle->add_checked_callback([key, base_name](bool checked) {
                    rush2::input::set_controller_enabled(key, base_name, checked);
                });
            }
            for (const auto& d : disabled) {
                bool connected = false;
                for (const auto& c : controllers) {
                    connected = connected || c.key == d.key;
                }
                if (connected) {
                    continue;
                }
                std::string name = (d.name.empty() ? std::string("Controller") : d.name) + " (not connected)";
                Toggle* toggle = add_toggle_line(name, "Off", false);
                std::string key = d.key;
                std::string base_name = d.name;
                toggle->add_checked_callback([key, base_name](bool checked) {
                    rush2::input::set_controller_enabled(key, base_name, checked);
                });
            }

            // The keyboard, a controller like the others.
            bool keyboard_on = rush2::input::get_keyboard_enabled();
            Toggle* keyboard_toggle = add_toggle_line("Keyboard",
                !keyboard_on ? "Off" : rush2::input::get_keyboard_port() < 0 ? "Press a key to use it"
                : port_status(rush2::input::get_keyboard_port()), keyboard_on);
            keyboard_toggle->add_checked_callback([](bool checked) {
                rush2::input::set_keyboard_enabled(checked);
            });
        }
    };
}

void rush2::controls_tab::create_tab() {
    add_profile_options();
    recompui::config::create_tab(tab_name, tab_id, [](ContextId context, Element* parent) {
        context.create_element<ControlsPage>(parent);
    });
}
