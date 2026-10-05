// Players tab: which controller and keyboard drive each player (src/input.cpp), players 1 and 2's Rush 2049 wings
// (src/wings.cpp) and the 2 player split screen layout (src/splitscreen.cpp).
//
// Each player has an input choice (Auto, Keyboard, None, or one of the connected controllers; a chosen controller
// that's unplugged stays listed). Keyboard gives the player only the keyboard; otherwise the keyboard stays with
// player 1 alongside its controller. Picking another player's controller swaps the two players' choices. The tab rebuilds itself when controllers connect or disconnect.
//
// Bindings are edited in the game's own Controller Setup screen (src/controls_menu.cpp), which replaces the frontend's
// Controls tab.

#include <string>
#include <vector>

#include "recompui/config.h"
#include "elements/ui_config_page.h"
#include "elements/ui_label.h"
#include "elements/ui_radio.h"
#include "elements/ui_select.h"

#include "rush2.h"
#include "wings.h"

namespace {
    using namespace recompui;
    using rush2::input::PortChoice;

    const std::string tab_id = "players";
    const std::string tab_name = "Players";

    const std::string description =
        "Choose the controller each player uses, or the keyboard.\n\n"
        "Auto gives the player the first controller that presses a button and isn't chosen for another player. "
        "A chosen controller is remembered and takes its player whenever it's connected. Choosing another player's "
        "controller swaps the two. The keyboard also drives player 1 unless another player chooses it.\n\n"
        "Buttons are set in the game's Controller Setup screen (Options, or the pause menu during a race).\n\n"
        "<recomp-color primary>Wings</recomp-color> chooses players 1 and 2's Rush 2049 wings, as on Rush 2049's car "
        "setup screen: Style 1 steers in the air, Style 2 steers harder and glides but slows the car, Style 3 steers "
        "hardest and glides farthest. Wings are turned on in the Games tab.\n\n"
        "<recomp-color primary>Split Screen</recomp-color> sets how the screen is split in 2 player races: Top and "
        "Bottom matches the original game, Side by Side gives each player half of the screen's width. It takes effect "
        "at the start of the next race.";

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

    // A player's input list. While it's open, each step through the options is a selection (RmlUi's select), so the
    // page waits for it to close before applying the choice.
    class PlayerSelect : public Select {
    public:
        using Select::Select;
        // Select only tracks this while it's being updated, which nothing starts, so ask the select itself.
        bool is_list_open() { return get_element_with_tag_name("selectvalue").is_pseudo_class_set("checked"); }
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
                // Applying a choice or rebuilding the rows would close an open list, so both wait for it.
                bool any_open = false;
                for (PlayerSelect* select : selects) {
                    any_open = any_open || select->is_list_open();
                }
                if (!any_open) {
                    for (int port = 0; port < (int)pending.size(); port++) {
                        if (!pending[port].empty()) {
                            apply_choice(port, pending[port]);
                            pending[port].clear();
                        }
                    }
                    if (current_state() != rendered_state) {
                        // The rebuilt list of the player whose list had focus takes it back.
                        int refocus = -1;
                        for (size_t i = 0; i < selects.size(); i++) {
                            if (selects[i]->is_style_enabled(focus_state)) {
                                refocus = (int)i;
                            }
                        }
                        render();
                        if (refocus >= 0 && refocus < (int)selects.size()) {
                            selects[refocus]->focus();
                        }
                    }
                }
                queue_update();
            }
        }

    private:
        Element* rows = nullptr;
        std::string rendered_state;
        std::vector<PlayerSelect*> selects;
        std::vector<std::string> pending = std::vector<std::string>(rush2::input::num_ports);  // Choices to apply.
        std::vector<rush2::input::ControllerInfo> shown_controllers;

        void apply_choice(int port, const std::string& value) {
            PortChoice current = rush2::input::get_port_choice(port);
            bool keyboard_only = rush2::input::get_keyboard_port() == port && current.kind == PortChoice::Kind::None;
            PortChoice c{};
            if (value == "keyboard") {
                rush2::input::set_keyboard_port(port);
            }
            else if (keyboard_only) {
                // The keyboard goes back to player 1, alongside player 1's controller.
                rush2::input::set_keyboard_port(0);
            }
            if (value == "none" || value == "keyboard") {
                c.kind = PortChoice::Kind::None;
            }
            else if (value.rfind("c:", 0) == 0) {
                c.kind = PortChoice::Kind::Controller;
                c.controller_key = value.substr(2);
                c.controller_name = current.controller_name;
                for (const auto& info : shown_controllers) {
                    if (info.key == c.controller_key) {
                        c.controller_name = info.name;
                    }
                }
            }
            rush2::input::set_port_choice(port, c);
        }

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
            return state + "|" + std::to_string(rush2::input::get_keyboard_port()) + "|" +
                std::to_string(rush2::wings::rom_available());
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
            selects.clear();

            std::vector<rush2::input::ControllerInfo> controllers = rush2::input::get_controllers();
            shown_controllers = controllers;

            for (int port = 0; port < rush2::input::num_ports; port++) {
                Element* row = add_row(rows, "Player " + std::to_string(port + 1));
                PortChoice choice = rush2::input::get_port_choice(port);

                // Keyboard: the keyboard alone (no controller). A player with the keyboard and a controller choice
                // (player 1 by default) shows the choice, and the status says the keyboard is also theirs.
                bool has_keyboard = rush2::input::get_keyboard_port() == port;
                bool keyboard_only = has_keyboard && choice.kind == PortChoice::Kind::None;
                std::vector<SelectOption> options = {
                    { "Auto", "auto" },
                    { "Keyboard", "keyboard" },
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

                std::string selected = keyboard_only ? "keyboard"
                    : choice.kind == PortChoice::Kind::Auto ? "auto"
                    : choice.kind == PortChoice::Kind::None ? "none" : "c:" + choice.controller_key;
                PlayerSelect* select = context.create_element<PlayerSelect>(row, options, selected);
                selects.push_back(select);
                select->add_change_callback([this, port](SelectOption& option, int) {
                    pending[port] = option.value;
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
                if (keyboard_only) {
                    status = "Using the keyboard";
                }
                else if (has_keyboard) {
                    status += live >= 0 ? ", and the keyboard" : ". Also using the keyboard";
                }
                Label* status_label = context.create_element<Label>(row, status, theme::Typography::Body);
                status_label->set_color(theme::color::TextDim);
            }

            // Wing styles: players 1 and 2, once the Rush 2049 ROM is there.
            if (rush2::wings::rom_available()) {
                Element* row = add_row(rows, "Wings");
                for (int player = 0; player < 2; player++) {
                    Element* line = context.create_element<Element>(row, 0, "div", false);
                    line->set_display(Display::Flex);
                    line->set_flex_direction(FlexDirection::Row);
                    line->set_align_items(AlignItems::Center);
                    line->set_gap(16.0f);
                    line->set_as_navigation_container(NavigationType::Horizontal);
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
