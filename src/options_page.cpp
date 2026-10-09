// A settings page built from option rows of any configs (include/options_page.h). The General, Graphics and Games tabs
// use it to order their options under headings, which the frontend's options menu can't do.

#include <algorithm>

#include "recompui/recompui.h"
#include "recompui/config.h"
#include "elements/ui_label.h"

#include "options_page.h"

using namespace recompui;

namespace rush2::ui {
    FocusRow::FocusRow(ResourceId rid, Element* parent) : Element(rid, parent, Events(EventType::Focus), "div", false) {
        set_width(100.0f, Unit::Percent);
    }

    void FocusRow::process_event(const Event& e) {
        if (e.type == EventType::Focus && std::get<EventFocus>(e.variant).active) {
            scroll_into_view();
        }
    }

    OptionsPage::OptionsPage(ResourceId rid, Element* parent, const std::string& description)
        : ConfigPage(rid, parent, Events(EventType::Hover, EventType::Update, EventType::MenuAction)),
          default_description(description) {
        ContextId context = get_current_context();
        set_as_navigation_container(NavigationType::Vertical);

        Element* left = body->get_left();
        left->set_padding(0.0f);
        left->set_display(Display::Block);
        left->set_position(Position::Relative);
        left->set_height(100.0f, Unit::Percent);
        list = context.create_element<Element>(left, 0, "div", false);
        list->set_display(Display::Block);
        list->set_width(100.0f, Unit::Percent);
        list->set_min_height(100.0f, Unit::Percent);
        list->set_max_height(100.0f, Unit::Percent);
        list->set_padding(16.0f);
        list->set_overflow_y(Overflow::Auto);
        list->set_as_navigation_container(NavigationType::Vertical);

        description_text = context.create_element<Element>(body->get_right(), 0, "p", true);
        description_text->set_typography(theme::Typography::Body);
        description_text->set_line_height(28.0f);
        description_text->set_padding(8.0f);
        show_description(nullptr, "");

        queue_update();
    }

    OptionsPage::Heading OptionsPage::add_heading(const std::string& title, const std::string& note) {
        ContextId context = get_current_context();
        // Some pages put buttons in the heading (Select ROM), so it scrolls into view like any row of buttons.
        Element* row = context.create_element<FocusRow>(list);
        row->set_display(Display::Flex);
        row->set_flex_direction(FlexDirection::Row);
        row->set_align_items(AlignItems::Center);
        row->set_gap(16.0f);
        row->set_width(100.0f, Unit::Percent);
        row->set_padding_left(12.0f);
        row->set_padding_right(12.0f);
        row->set_padding_top(rows.empty() && headings == 0 ? 4.0f : 24.0f);
        headings++;
        row->set_padding_bottom(8.0f);
        row->set_margin_bottom(4.0f);
        row->set_border_bottom_width(1.0f);
        row->set_border_bottom_color(theme::color::Border);
        row->set_as_navigation_container(NavigationType::Horizontal);

        Element* text = context.create_element<Element>(row, 0, "div", false);
        text->set_display(Display::Flex);
        text->set_flex_direction(FlexDirection::Column);
        text->set_flex_grow(1.0f);
        text->set_gap(4.0f);
        context.create_element<Label>(text, title, theme::Typography::LabelLG);
        Label* note_label = context.create_element<Label>(text, note, theme::Typography::Body);
        note_label->set_color(theme::color::TextDim);
        if (note.empty()) {
            note_label->set_display(Display::None);
        }
        return { row, note_label };
    }

    Element* OptionsPage::add_row() {
        ContextId context = get_current_context();
        Element* row = context.create_element<FocusRow>(list);
        row->set_display(Display::Flex);
        row->set_flex_direction(FlexDirection::Row);
        row->set_align_items(AlignItems::Center);
        row->set_gap(16.0f);
        row->set_padding_left(12.0f);
        row->set_padding_right(12.0f);
        row->set_padding_top(8.0f);
        row->set_padding_bottom(16.0f);
        row->set_as_navigation_container(NavigationType::Horizontal);
        return row;
    }

    void OptionsPage::add_option(recomp::config::Config& config, const std::string& option_id) {
        ContextId context = get_current_context();
        recomp::config::Config* cfg = &config;
        size_t index = config.get_config_schema().options_by_id.at(option_id);
        const recomp::config::ConfigOption& option = config.get_option(index);

        set_option_value_t set_value = [this, cfg](const std::string& id, recomp::config::ConfigValueVariant value) {
            cfg->set_option_value(id, value);
            if (apply_button != nullptr) {
                apply_button->set_enabled(any_dirty());
            }
        };
        on_option_hover_t on_hover = [this, cfg](const std::string& id) {
            show_description(cfg, id);
        };

        ConfigOptionElement* element = nullptr;
        switch (option.type) {
            case recomp::config::ConfigOptionType::Enum:
                element = context.create_element<ConfigOptionEnum>(list, option_id, index, cfg, set_value, on_hover);
                break;
            case recomp::config::ConfigOptionType::Number:
                element = context.create_element<ConfigOptionNumber>(list, option_id, index, cfg, set_value, on_hover);
                break;
            case recomp::config::ConfigOptionType::String:
                element = context.create_element<ConfigOptionString>(list, option_id, index, cfg, set_value, on_hover);
                break;
            case recomp::config::ConfigOptionType::Bool:
                element = context.create_element<ConfigOptionBool>(list, option_id, index, cfg, set_value, on_hover);
                break;
            default:
                return;
        }
        element->update_disabled();
        element->update_hidden();
        rows.push_back({ cfg, element });

        if (std::find(configs.begin(), configs.end(), cfg) == configs.end()) {
            configs.push_back(cfg);
            // Updates reported before the page opened are already shown by the new elements.
            cfg->clear_config_option_updates();
            if (cfg->requires_confirmation && apply_button == nullptr) {
                add_footer();
                footer->set_as_navigation_container(NavigationType::Horizontal);
                apply_button = context.create_element<Button>(footer->get_right(), "Apply", ButtonStyle::Secondary);
                apply_button->set_enabled(false);
                apply_button->set_as_primary_focus();
                apply_button->add_pressed_callback([this]() {
                    for (recomp::config::Config* c : configs) {
                        if (c->requires_confirmation) {
                            c->save_config();
                        }
                    }
                    apply_button->set_enabled(any_dirty());
                });
            }
        }
    }

    void OptionsPage::set_default_description(const std::string& text) {
        default_description = text;
        if (shown_config == nullptr) {
            show_description(nullptr, "");
        }
    }

    void OptionsPage::show_description(recomp::config::Config* config, const std::string& option_id) {
        shown_config = config;
        shown_option = option_id;
        description_text->set_text_unsafe(config != nullptr ? config->get_option(option_id).description : default_description);
    }

    bool OptionsPage::any_dirty() const {
        for (recomp::config::Config* c : configs) {
            if (c->requires_confirmation && c->is_dirty()) {
                return true;
            }
        }
        return false;
    }

    // The frontend's options menu's render updates (ui_config_page_options_menu.cpp) for one of the page's configs.
    void OptionsPage::apply_updates(recomp::config::Config* config) {
        using UpdateType = recomp::config::ConfigOptionUpdateType;
        for (auto& update : config->get_config_option_updates()) {
            const std::string& id = config->get_option(update.option_index).id;
            for (const Row& row : rows) {
                if (row.config != config || row.element->get_option_id() != id) {
                    continue;
                }
                for (UpdateType type : update.updates) {
                    switch (type) {
                        case UpdateType::Disabled:
                            row.element->update_disabled();
                            break;
                        case UpdateType::Hidden:
                            row.element->update_hidden();
                            break;
                        case UpdateType::EnumDetails:
                            static_cast<ConfigOptionEnum*>(row.element)->update_enum_details();
                            break;
                        case UpdateType::EnumDisabled:
                            static_cast<ConfigOptionEnum*>(row.element)->update_enum_disabled();
                            break;
                        case UpdateType::Value:
                            row.element->update_value();
                            break;
                        case UpdateType::Description:
                            if (shown_config == config && shown_option == id) {
                                show_description(config, id);
                            }
                            break;
                    }
                }
            }
        }
        config->clear_config_option_updates();
    }

    void OptionsPage::process_event(const Event& e) {
        switch (e.type) {
            case EventType::Hover:
                if (!std::get<EventHover>(e.variant).active) {
                    show_description(nullptr, "");
                }
                break;
            case EventType::Update:
                for (recomp::config::Config* c : configs) {
                    apply_updates(c);
                }
                if (apply_button != nullptr && apply_button->is_enabled() != any_dirty()) {
                    apply_button->set_enabled(any_dirty());
                }
                on_update();
                for (const auto& callback : update_callbacks) {
                    callback();
                }
                queue_update();
                break;
            case EventType::MenuAction:
                if (std::get<EventMenuAction>(e.variant).action == MenuAction::Apply) {
                    for (recomp::config::Config* c : configs) {
                        if (c->requires_confirmation) {
                            c->save_config();
                        }
                    }
                }
                break;
            default:
                break;
        }
    }

    // The frontend's can_close for its own config tabs (ui_config.cpp).
    bool confirm_close(const std::string& config_id, const std::string& name, TabCloseContext close_context) {
        recomp::config::Config& config = recompui::config::get_config(config_id);
        if (!config.requires_confirmation || !config.is_dirty()) {
            return true;
        }
        recompui::open_choice_prompt(
            name + " options have unapplied changes.",
            "Would you like to apply or discard the changes?",
            "Apply",
            "Discard",
            [config_id, close_context]() {
                recompui::config::get_config(config_id).save_config();
                if (close_context == TabCloseContext::ModalClose) {
                    recompui::config::close();
                }
            },
            [config_id, close_context]() {
                recompui::config::get_config(config_id).revert_temp_config();
                if (close_context == TabCloseContext::ModalClose) {
                    recompui::config::close();
                }
            },
            ButtonStyle::Success,
            ButtonStyle::Danger,
            true
        );
        return false;
    }
}
