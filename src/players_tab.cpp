// Players tab: which controller and keyboard drive each player (src/input.cpp).
//
// Each player has a controller choice (Auto, None, or one of the connected controllers; a chosen controller that's
// unplugged stays listed) and the keyboard goes to one player or none. Picking another player's controller swaps the
// two players' choices. The tab rebuilds itself when controllers connect or disconnect.
//
// Bindings are edited in the game's own Controller Setup screen (src/controls_menu.cpp), which replaces the frontend's
// Controls tab.

#include <string>
#include <vector>

#include "recompui/config.h"
#include "elements/ui_config_page.h"
#include "elements/ui_label.h"
#include "elements/ui_select.h"

#include "rush2.h"

namespace {
    using namespace recompui;
    using rush2::input::PortChoice;

    const std::string tab_id = "players";
    const std::string tab_name = "Players";

    const std::string description =
        "Choose the controller each player uses.\n\n"
        "Auto gives the player the first controller that presses a button and isn't chosen for the other player. "
        "A chosen controller is remembered and takes its player whenever it's connected. Choosing the other player's "
        "controller swaps the two.\n\n"
        "Buttons are set in the game's Controller Setup screen (Options, or the pause menu during a race).";

    class PlayersPage : public ConfigPage {
    public:
        PlayersPage(ResourceId rid, Element* parent) : ConfigPage(rid, parent, Events(EventType::Update)) {
            ContextId context = get_current_context();
            set_as_navigation_container(NavigationType::Vertical);

            rows = context.create_element<Element>(body->get_left(), 0, "div", false);
            rows->set_display(Display::Flex);
            rows->set_flex_direction(FlexDirection::Column);
            rows->set_width(100.0f, Unit::Percent);
            rows->set_as_navigation_container(NavigationType::Vertical);

            Element* text = context.create_element<Element>(body->get_right(), 0, "p", true);
            text->set_typography(theme::Typography::Body);
            text->set_line_height(28.0f);
            text->set_padding(8.0f);
            text->set_text(description);

            render();
            queue_update();
        }

    protected:
        std::string_view get_type_name() override { return "PlayersPage"; }

        void process_event(const Event& e) override {
            if (e.type == EventType::Update) {
                if (current_state() != rendered_state) {
                    render();
                }
                queue_update();
            }
        }

    private:
        Element* rows = nullptr;
        std::string rendered_state;

        // Everything the rows show, so they're only rebuilt when it changes.
        std::string current_state() {
            std::string state;
            for (const auto& c : rush2::input::get_controllers()) {
                state += std::to_string(c.joystick_id) + c.key + ";";
            }
            for (int port = 0; port < rush2::input::num_ports; port++) {
                PortChoice choice = rush2::input::get_port_choice(port);
                state += "|" + std::to_string((int)choice.kind) + choice.controller_key + ":" +
                    std::to_string(rush2::input::get_port_controller(port));
            }
            return state + "|" + std::to_string(rush2::input::get_keyboard_port());
        }

        Element* add_row(Element* parent, const std::string& name) {
            ContextId context = get_current_context();
            Element* row = context.create_element<Element>(parent, 0, "div", false);
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

            std::vector<rush2::input::ControllerInfo> controllers = rush2::input::get_controllers();

            for (int port = 0; port < rush2::input::num_ports; port++) {
                Element* row = add_row(rows, "Player " + std::to_string(port + 1));
                PortChoice choice = rush2::input::get_port_choice(port);

                std::vector<SelectOption> options = {
                    { "Auto", "auto" },
                    { "None", "none" },
                };
                bool chosen_connected = false;
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
                    options.emplace_back(name, "c:" + controllers[i].key);
                    chosen_connected = chosen_connected || controllers[i].key == choice.controller_key;
                }
                if (choice.kind == PortChoice::Kind::Controller && !chosen_connected) {
                    std::string name = choice.controller_name.empty() ? "Controller" : choice.controller_name;
                    options.emplace_back(name + " (not connected)", "c:" + choice.controller_key);
                }

                std::string selected = choice.kind == PortChoice::Kind::Auto ? "auto"
                    : choice.kind == PortChoice::Kind::None ? "none" : "c:" + choice.controller_key;
                Select* select = context.create_element<Select>(row, options, selected);
                select->add_change_callback([port, controllers](SelectOption& option, int) {
                    PortChoice c{};
                    if (option.value == "none") {
                        c.kind = PortChoice::Kind::None;
                    }
                    else if (option.value.rfind("c:", 0) == 0) {
                        c.kind = PortChoice::Kind::Controller;
                        c.controller_key = option.value.substr(2);
                        c.controller_name = rush2::input::get_port_choice(port).controller_name;
                        for (const auto& info : controllers) {
                            if (info.key == c.controller_key) {
                                c.controller_name = info.name;
                            }
                        }
                    }
                    rush2::input::set_port_choice(port, c);
                });

                // What the player has right now.
                std::string status = "No controller";
                int32_t live = rush2::input::get_port_controller(port);
                for (const auto& info : controllers) {
                    if (info.joystick_id == live) {
                        status = "Using " + info.name;
                    }
                }
                if (live < 0 && choice.kind == PortChoice::Kind::Auto) {
                    status = "No controller yet: press a button on one to join";
                }
                Label* status_label = context.create_element<Label>(row, status, theme::Typography::Body);
                status_label->set_color(theme::color::TextDim);
            }

            Element* row = add_row(rows, "Keyboard");
            std::vector<SelectOption> options;
            for (int port = 0; port < rush2::input::num_ports; port++) {
                options.emplace_back("Player " + std::to_string(port + 1), std::to_string(port));
            }
            options.emplace_back("Off", "-1");
            Select* select = context.create_element<Select>(row, options, std::to_string(rush2::input::get_keyboard_port()));
            select->add_change_callback([](SelectOption& option, int) {
                rush2::input::set_keyboard_port(std::stoi(option.value));
            });
        }
    };
}

void rush2::players::create_tab() {
    recompui::config::create_tab(tab_name, tab_id, [](ContextId context, Element* parent) {
        context.create_element<PlayersPage>(parent);
    });
}
