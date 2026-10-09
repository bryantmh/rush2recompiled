// Players tab: which controllers and the keyboard may play (src/input.cpp), players 1 and 2's Rush 2049 wings
// (src/wings.cpp), the 2 player split screen layout (src/splitscreen.cpp) and the computer cars' cars and paint
// (src/npc_cars.cpp).
//
// Controllers aren't given to players here: in the game, whoever presses START becomes the next player. Each
// controller (connected, or disabled and remembered) and the keyboard has a toggle that enables it. The tab rebuilds
// itself when controllers
// connect or disconnect, or a device's port or player changes.
//
// Bindings are edited in the game's own Controller Setup screen (src/controls_menu.cpp), which replaces the frontend's
// Controls tab.

#include <string>
#include <vector>

#include "recompui/config.h"
#include "elements/ui_config_page.h"
#include "elements/ui_label.h"
#include "elements/ui_radio.h"
#include "elements/ui_toggle.h"

#include "npc_cars.h"
#include "rush2.h"
#include "wings.h"

namespace {
    using namespace recompui;

    const std::string tab_id = "players";
    const std::string tab_name = "Players";

    const std::string description =
        "Choose which controllers can play. All of them can unless turned off here.\n\n"
        "Players join in the game: player 1 presses START on the title screen, and players 2, 3 and 4 press START on "
        "Select Player. A controller can join once it has pressed a button.\n\n"
        "Buttons are set in the game's Controller Setup screen (Options, or the pause menu during a race).\n\n"
        "<recomp-color primary>Wings</recomp-color> chooses players 1 and 2's Rush 2049 wings, as on Rush 2049's car "
        "setup screen: Style 1 steers in the air, Style 2 steers harder and glides but slows the car, Style 3 steers "
        "hardest and glides farthest. Wings are turned on in the Games tab.\n\n"
        "<recomp-color primary>Split Screen</recomp-color> sets how the screen is split in 2 player races: Top and "
        "Bottom matches the original game, Side by Side gives each player half of the screen's width. It takes effect "
        "at the start of the next race.\n\n"
        "<recomp-color primary>AI Opponents</recomp-color> chooses each computer car's car, stripe, rims and colors. "
        "Opponent 1 is the first computer car on the grid. Random keeps the game's own pick. The New York Cabs cheat "
        "still turns them into taxis, and ghost races keep their recorded cars. Reset puts every opponent back to "
        "Random. Takes effect at the next race.";

    // A row of the list: scrolls itself into view when something in it takes focus, so the list follows the d-pad.
    class ListRow : public Element {
    public:
        ListRow(ResourceId rid, Element* parent) : Element(rid, parent, Events(EventType::Focus), "div", false) {}

    protected:
        std::string_view get_type_name() override { return "PlayersListRow"; }

        void process_event(const Event& e) override {
            if (e.type == EventType::Focus && std::get<EventFocus>(e.variant).active) {
                scroll_into_view();
            }
        }
    };

    class PlayersPage : public ConfigPage {
    public:
        PlayersPage(ResourceId rid, Element* parent) : ConfigPage(rid, parent, Events(EventType::Update)) {
            ContextId context = get_current_context();
            set_as_navigation_container(NavigationType::Vertical);

            Element* left = body->get_left();
            left->set_padding(0.0f);
            left->set_display(Display::Block);
            left->set_position(Position::Relative);
            left->set_height(100.0f, Unit::Percent);
            rows = context.create_element<Element>(left, 0, "div", false);
            rows->set_display(Display::Block);
            rows->set_width(100.0f, Unit::Percent);
            rows->set_min_height(100.0f, Unit::Percent);
            rows->set_max_height(100.0f, Unit::Percent);
            rows->set_padding(16.0f);
            rows->set_overflow_y(Overflow::Auto);
            rows->set_as_navigation_container(NavigationType::Vertical);

            Element* text = context.create_element<Element>(body->get_right(), 0, "p", true);
            text->set_typography(theme::Typography::Body);
            text->set_line_height(28.0f);
            text->set_padding(8.0f);
            std::string html;
            for (char c : description) {
                html += c == '\n' ?std::string("<br/>") : std::string(1, c);
            }
            text->set_text_unsafe(html);

            render();
            queue_update();
        }

    protected:
        std::string_view get_type_name() override { return "PlayersPage"; }

        void process_event(const Event& e) override {
            if (e.type == EventType::Update) {
                if (current_state() != rendered_state) {
                    // The rebuilt toggle of the one that had focus takes it back.
                    int refocus = -1;
                    for (size_t i = 0; i < toggles.size(); i++) {
                        if (toggles[i]->is_style_enabled(focus_state)) {
                            refocus = (int)i;
                        }
                    }
                    render();
                    if (refocus >= 0 && refocus < (int)toggles.size()) {
                        toggles[refocus]->focus();
                    }
                }
                queue_update();
            }
        }

    private:
        Element* rows = nullptr;
        std::string rendered_state;
        std::vector<Toggle*> toggles;

        // Everything the rows show, so they're only rebuilt when it changes.
        std::string current_state() {
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
                std::to_string(rush2::input::get_keyboard_enabled()) +
                std::to_string(rush2::wings::rom_available());
        }

        // What a device is doing right now, from its port (-1 for none).
        static std::string port_status(int port) {
            if (port < 0) {
                return "Press a button on it to use it";
            }
            int player = rush2::input::port_player(port);
            if (player >= 0) {
                return "Player " + std::to_string(player + 1);
            }
            return "Ready: press START in the game to join";
        }

        // A line of the list: a name and status on the left and a toggle on the right.
        Toggle* add_toggle_line(Element* parent, const std::string& name, const std::string& status, bool checked) {
            ContextId context = get_current_context();
            Element* line = context.create_element<ListRow>(parent);
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

        Element* add_row(Element* parent, const std::string& name) {
            ContextId context = get_current_context();
            Element* row = context.create_element<ListRow>(parent);
            row->set_display(Display::Flex);
            row->set_flex_direction(FlexDirection::Column);
            row->set_padding(12.0f);
            row->set_gap(8.0f);
            row->set_width(100.0f, Unit::Percent);
            row->set_as_navigation_container(NavigationType::Vertical);
            context.create_element<Label>(row, name, theme::Typography::LabelMD);
            return row;
        }

        void render() {
            ContextId context = get_current_context();
            rendered_state = current_state();
            rows->clear_children();
            toggles.clear();

            // Controllers: the connected ones, then the disabled ones that aren't connected.
            Element* controllers_row = add_row(rows, "Controllers");
            std::vector<rush2::input::ControllerInfo> controllers = rush2::input::get_controllers();
            std::vector<rush2::input::DisabledController> disabled = rush2::input::get_disabled_controllers();
            if (controllers.empty() && disabled.empty()) {
                Label* none = context.create_element<Label>(controllers_row, "No game controllers connected", theme::Typography::Body);
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
                Toggle* toggle = add_toggle_line(controllers_row, name, enabled ? port_status(port) : "Off", enabled);
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
                Toggle* toggle = add_toggle_line(controllers_row, name, "Off", false);
                std::string key = d.key;
                std::string base_name = d.name;
                toggle->add_checked_callback([key, base_name](bool checked) {
                    rush2::input::set_controller_enabled(key, base_name, checked);
                });
            }

            // The keyboard, a controller like the others.
            bool keyboard_on = rush2::input::get_keyboard_enabled();
            Toggle* keyboard_toggle = add_toggle_line(controllers_row, "Keyboard",
                !keyboard_on ? "Off" : rush2::input::get_keyboard_port() < 0 ? "Press a key to use it"
                : port_status(rush2::input::get_keyboard_port()), keyboard_on);
            keyboard_toggle->add_checked_callback([](bool checked) {
                rush2::input::set_keyboard_enabled(checked);
            });

            // Wing styles: players 1 and 2, once the Rush 2049 ROM is there.
            if (rush2::wings::rom_available()) {
                Element* row = add_row(rows, "Wings");
                for (int player = 0; player < 2; player++) {
                    Element* line = context.create_element<Element>(row, 0, "div", false);

            rush2::npc_cars::add_section(rows);
                    line->set_display(Display::Flex);
                    line->set_flex_direction(FlexDirection::Row);
                    line->set_align_items(AlignItems::Center);
                    line->set_gap(16.0f);
                    line->set_as_navigation_container(NavigationType::Horizontal);
    rush2::npc_cars::init_config();
                    Label* name = context.create_element<Label>(line, "Player " + std::to_string(player + 1), theme::Typography::LabelSM);
                    name->set_color(theme::color::TextDim);
                    name->set_min_width(80.0f);
                    Radio* styles = context.create_element<Radio>(line);
                    styles->add_option("Style 1");
                    styles->add_option("Style 2");
                    styles->add_option("Style 3");
                    styles->set_index((uint32_t)rush2::wings::get_style_option(player));
                    styles->add_index_changed_callback([player](uint32_t index) {
                        rush2::wings::set_style_option(player, (int)index);
                    });
                }
            }

            using rush2::splitscreen::Layout;
            Element* split_row = add_row(rows, "Split Screen");
            Radio* layouts = context.create_element<Radio>(split_row);
            layouts->add_option("Top and Bottom");
            layouts->add_option("Side by Side");
            layouts->set_index(rush2::splitscreen::get_layout_option() == Layout::SideBySide ? 1 : 0);
            layouts->add_index_changed_callback([](uint32_t index) {
                rush2::splitscreen::set_layout_option(index == 1 ? Layout::SideBySide : Layout::TopBottom);
            });
        }
    };
}

void rush2::players::create_tab() {
    recompui::config::create_tab(tab_name, tab_id, [](ContextId context, Element* parent) {
        context.create_element<PlayersPage>(parent);
    });
}
