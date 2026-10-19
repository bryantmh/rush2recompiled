// A settings page built from option rows of any configs (include/options_page.h). The General, Graphics and Games tabs
// use it to order their options under headings in columns, which the frontend's options menu can't do.

#include <algorithm>

#include "recompui/recompui.h"
#include "recompui/config.h"
#include "elements/ui_label.h"
#include "elements/ui_select.h"
#include "elements/ui_slider.h"
#include "elements/ui_text_input.h"
#include "elements/ui_toggle.h"

#include "options_page.h"

using namespace recompui;

namespace rush2::ui {
    FocusRow::FocusRow(ResourceId rid, Element* parent, std::function<void()> on_show)
        : Element(rid, parent, Events(EventType::Focus, EventType::Hover), "div", false), on_show(std::move(on_show)) {
        set_width(100.0f, Unit::Percent);
    }

    // RmlUi sends focus and hover to every element of the focus and hover chains, so the row hears them for its
    // children too.
    void FocusRow::process_event(const Event& e) {
        if (e.type == EventType::Focus && std::get<EventFocus>(e.variant).active) {
            scroll_into_view();
            if (on_show) {
                on_show();
            }
        }
        else if (e.type == EventType::Hover && std::get<EventHover>(e.variant).active && on_show) {
            on_show();
        }
    }

    // An option of the column layout on one line: its name (with the enum's details and any status under it), then its
    // control. It follows the config like the frontend's option elements (ui_config_option.cpp): the page passes on
    // the config's updates.
    class CompactOption : public FocusRow {
    public:
        using SetValue = std::function<void(const std::string&, recomp::config::ConfigValueVariant)>;

        CompactOption(ResourceId rid, Element* parent, recomp::config::Config* config, const std::string& option_id,
                      SetValue set_value, std::function<void()> on_show)
            : FocusRow(rid, parent, std::move(on_show)), config(config), option_id(option_id),
              index(config->get_config_schema().options_by_id.at(option_id)), set_value(std::move(set_value)) {
            ContextId context = get_current_context();
            const recomp::config::ConfigOption& option = config->get_option(index);
            set_display(Display::Flex);
            set_flex_direction(FlexDirection::Row);
            set_align_items(AlignItems::Center);
            set_gap(12.0f);
            // A little taller than a select, so stacked selects don't touch.
            set_min_height(row_height);
            set_padding_top(1.0f);
            set_padding_bottom(1.0f);
            set_padding_left(4.0f);
            set_padding_right(4.0f);
            set_as_navigation_container(NavigationType::Horizontal);

            text = context.create_element<Element>(this, 0, "div", false);
            text->set_flex_grow(1.0f);
            text->set_flex_shrink(1.0f);
            name = context.create_element<Label>(text, option.name, theme::Typography::LabelMD);
            name->set_display(Display::Block);

            switch (option.type) {
                case recomp::config::ConfigOptionType::Bool: {
                    toggle = context.create_element<Toggle>(this, ToggleSize::Medium);
                    toggle->set_flex_shrink(0.0f);
                    toggle->add_checked_callback([this](bool checked) {
                        if (std::get<bool>(value()) != checked) {
                            this->set_value(this->option_id, checked);
                        }
                    });
                    break;
                }
                case recomp::config::ConfigOptionType::Enum: {
                    details = context.create_element<Label>(text, "", theme::Typography::LabelXS);
                    details->set_display(Display::None);
                    details->set_color(theme::color::Primary);
                    std::vector<SelectOption> choices;
                    for (const auto& choice : std::get<recomp::config::ConfigOptionEnum>(option.variant).options) {
                        choices.emplace_back(choice.name, std::to_string(choice.value));
                    }
                    // The select fills its parent's width, so it gets a parent of its own.
                    Element* holder = context.create_element<Element>(this, 0, "div", false);
                    holder->set_width(control_width);
                    holder->set_flex_shrink(0.0f);
                    select = context.create_element<Select>(holder, choices, std::to_string(std::get<uint32_t>(value())));
                    select->set_tab_index_auto();
                    select->add_change_callback([this](SelectOption& choice, int) {
                        uint32_t chosen = (uint32_t)std::stoul(choice.value);
                        if (std::get<uint32_t>(value()) != chosen) {
                            this->set_value(this->option_id, chosen);
                        }
                    });
                    break;
                }
                case recomp::config::ConfigOptionType::Number: {
                    // As wide as a select, so the column's controls line up.
                    const auto& number = std::get<recomp::config::ConfigOptionNumber>(option.variant);
                    Element* holder = context.create_element<Element>(this, 0, "div", false);
                    holder->set_width(control_width);
                    holder->set_flex_shrink(0.0f);
                    slider = context.create_element<Slider>(holder, number.percent ? SliderType::Percent : SliderType::Double);
                    slider->set_width(100.0f, Unit::Percent);
                    slider->set_min_value(number.min);
                    slider->set_max_value(number.max);
                    slider->set_step_value(number.step);
                    slider->set_precision(number.precision);
                    slider->add_value_changed_callback([this](double v) {
                        if (std::get<double>(value()) != v) {
                            this->set_value(this->option_id, v);
                        }
                    });
                    break;
                }
                case recomp::config::ConfigOptionType::String: {
                    Element* holder = context.create_element<Element>(this, 0, "div", false);
                    holder->set_width(55.0f, Unit::Percent);
                    holder->set_flex_shrink(0.0f);
                    text_input = context.create_element<TextInput>(holder);
                    text_input->set_width(100.0f, Unit::Percent);
                    text_input->add_text_changed_callback([this](const std::string& s) {
                        if (std::get<std::string>(value()) != s) {
                            this->set_value(this->option_id, s);
                        }
                    });
                    break;
                }
                default:
                    break;
            }
            update_value();
            update_enum_details();
            update_disabled();
            update_hidden();
        }

        const std::string& get_option_id() const { return option_id; }

        // Dim text under the name and control, in room the row keeps for it below them, so the name stays centered
        // on the control and showing or clearing the text moves nothing.
        Label* add_status() {
            constexpr float status_height = 24.0f;
            set_position(Position::Relative);
            set_padding_bottom(status_height + 1.0f);
            set_min_height(row_height + status_height);
            Label* label = get_current_context().create_element<Label>(this, "", theme::Typography::Body);
            label->set_position(Position::Absolute);
            label->set_left(4.0f);
            label->set_right(4.0f);
            label->set_bottom(2.0f);
            label->set_height(status_height);
            label->set_white_space(WhiteSpace::Nowrap);
            label->set_overflow(Overflow::Hidden);
            label->set_color(theme::color::TextDim);
            return label;
        }

        void update_value() {
            recomp::config::ConfigValueVariant v = value();
            if (toggle != nullptr) {
                toggle->set_checked(std::get<bool>(v));
            }
            else if (select != nullptr) {
                select->set_selection(std::to_string(std::get<uint32_t>(v)));
            }
            else if (slider != nullptr) {
                slider->set_value(std::get<double>(v));
            }
            else if (text_input != nullptr) {
                text_input->set_text(std::get<std::string>(v));
            }
        }

        void update_disabled() {
            bool enabled = !config->is_config_option_disabled(index);
            name->set_color(enabled ? theme::color::Text : theme::color::TextInactive);
            if (toggle != nullptr) toggle->set_enabled(enabled);
            if (slider != nullptr) slider->set_enabled(enabled);
            if (text_input != nullptr) text_input->set_enabled(enabled);
            if (select != nullptr) {
                select->set_enabled(enabled);
                update_enum_disabled();
            }
        }

        void update_hidden() {
            bool hidden = config->is_config_option_hidden(index);
            set_display(hidden ? Display::None : Display::Flex);
            set_enabled(!hidden);
        }

        void update_enum_details() {
            if (details == nullptr) {
                return;
            }
            const std::string& text = config->get_enum_option_details(index);
            details->set_text(text);
            details->set_display(text.empty() ? Display::None : Display::Block);
        }

        void update_enum_disabled() {
            if (select == nullptr) {
                return;
            }
            const auto& choices = config->get_option_config<recomp::config::ConfigOptionEnum>(index).options;
            for (size_t i = 0; i < choices.size(); i++) {
                select->set_option_enabled((int)i, !config->get_enum_option_disabled(index, (uint32_t)i));
            }
        }

    protected:
        std::string_view get_type_name() override { return "Rush2CompactOption"; }

    private:
        static constexpr float row_height = 50.0f;      // A select's 48, plus a little between stacked ones.
        static constexpr float control_width = 216.0f;  // A select's, or a slider's.
        recomp::config::Config* config;
        std::string option_id;
        size_t index;
        SetValue set_value;
        Element* text = nullptr;
        Label* name = nullptr;
        Label* details = nullptr;
        Toggle* toggle = nullptr;
        Select* select = nullptr;
        Slider* slider = nullptr;
        TextInput* text_input = nullptr;

        // Configs that need Apply show their unapplied value, as the frontend's option elements do.
        recomp::config::ConfigValueVariant value() const {
            return config->requires_confirmation ? config->get_temp_option_value(option_id) : config->get_option_value(option_id);
        }
    };

    OptionsPage::OptionsPage(ResourceId rid, Element* parent, const std::string& description, int column_count)
        : ConfigPage(rid, parent, Events(EventType::Hover, EventType::Update, EventType::MenuAction)),
          default_description(description) {
        ContextId context = get_current_context();
        set_as_navigation_container(NavigationType::Vertical);

        Element* left = body->get_left();
        if (column_count > 0) {
            // The columns, then the description across the bottom, placed in the page's side rather than flexed.
            constexpr float description_height = 124.0f;
            constexpr float column_gap = 32.0f;
            body->get_right()->set_display(Display::None);
            left->set_display(Display::Block);
            left->set_position(Position::Relative);
            left->set_height(100.0f, Unit::Percent);

            Element* grid = context.create_element<Element>(left, 0, "div", false);
            grid->set_position(Position::Absolute);
            grid->set_top(16.0f);
            grid->set_left(16.0f);
            grid->set_right(16.0f - column_gap * 0.5f);
            grid->set_bottom(description_height + 24.0f);
            grid->set_as_navigation_container(NavigationType::Horizontal);
            for (int i = 0; i < column_count; i++) {
                Element* col = context.create_element<Element>(grid, 0, "div", false);
                col->set_position(Position::Absolute);
                col->set_top(0.0f);
                col->set_bottom(0.0f);
                col->set_left(100.0f * i / column_count, Unit::Percent);
                col->set_width(100.0f / column_count, Unit::Percent);
                col->set_padding_right(column_gap * 0.5f);
                col->set_padding_left(i == 0 ? 0.0f : column_gap * 0.5f);
                col->set_overflow_y(Overflow::Auto);
                col->set_as_navigation_container(NavigationType::Vertical);
                columns.push_back(col);
                column_items.push_back(0);
            }
            list = columns[0];

            description_text = context.create_element<Element>(left, 0, "p", true);
            description_text->set_position(Position::Absolute);
            description_text->set_left(16.0f);
            description_text->set_right(16.0f);
            description_text->set_bottom(16.0f);
            description_text->set_height(description_height);
            description_text->set_typography(theme::Typography::Body);
            description_text->set_line_height(28.0f);
            description_text->set_padding_top(8.0f);
            description_text->set_overflow_y(Overflow::Auto);
            description_text->set_color(theme::color::TextDim);
            description_text->set_border_top_width(1.0f);
            description_text->set_border_top_color(theme::color::Border);
        }
        else {
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
        }
        show_description(nullptr, "");

        queue_update();
    }

    void OptionsPage::next_column() {
        if (column + 1 < columns.size()) {
            column++;
            list = columns[column];
        }
    }

    std::shared_ptr<ExpandPanel> OptionsPage::expand_panel() {
        if (!panel) {
            panel = std::make_shared<ExpandPanel>();
            panel->area = columns.empty() ? list : columns.back();
        }
        return panel;
    }

    std::function<void()> OptionsPage::text_shower(const std::string& text) {
        if (text.empty()) {
            return nullptr;
        }
        return [this, text]() { show_text(text); };
    }

    OptionsPage::Heading OptionsPage::add_heading(const std::string& title, const std::string& note, const std::string& description) {
        ContextId context = get_current_context();
        // Some pages put buttons in the heading (Select ROM), so it scrolls into view like any row of buttons.
        Element* row = context.create_element<FocusRow>(list, text_shower(description));
        row->set_display(Display::Flex);
        row->set_flex_direction(FlexDirection::Row);
        row->set_align_items(AlignItems::Center);
        row->set_gap(16.0f);
        row->set_width(100.0f, Unit::Percent);
        if (columns.empty()) {
            row->set_padding_left(12.0f);
            row->set_padding_right(12.0f);
            row->set_padding_top(rows.empty() && headings == 0 ? 4.0f : 24.0f);
            row->set_padding_bottom(8.0f);
        }
        else {
            // Like the Cheats tab's group titles: underlined, close above their options.
            row->set_padding_left(4.0f);
            row->set_padding_right(4.0f);
            row->set_padding_top(column_items[column] == 0 ? 0.0f : 16.0f);
            row->set_padding_bottom(6.0f);
            column_items[column]++;
        }
        headings++;
        row->set_margin_bottom(4.0f);
        row->set_border_bottom_width(1.0f);
        row->set_border_bottom_color(theme::color::Border);
        row->set_as_navigation_container(NavigationType::Horizontal);

        Element* text = context.create_element<Element>(row, 0, "div", false);
        text->set_display(Display::Flex);
        text->set_flex_direction(FlexDirection::Column);
        text->set_flex_grow(1.0f);
        text->set_gap(4.0f);
        Label* title_label = context.create_element<Label>(text, title, theme::Typography::LabelLG);
        Label* note_label = context.create_element<Label>(text, note, theme::Typography::Body);
        note_label->set_color(theme::color::TextDim);
        if (note.empty()) {
            note_label->set_display(Display::None);
        }
        return { row, note_label, title_label };
    }

    Label* OptionsPage::add_option_status(const std::string& option_id) {
        for (const Row& r : rows) {
            if (r.option_id != option_id) continue;
            if (r.compact != nullptr) {
                return r.compact->add_status();
            }
            Label* label = get_current_context().create_element<Label>(r.element, "", theme::Typography::Body);
            label->set_position(Position::Absolute);
            label->set_top(8.0f);
            label->set_right(12.0f);
            label->set_color(theme::color::TextDim);
            return label;
        }
        return nullptr;
    }

    Element* OptionsPage::get_option_row(const std::string& option_id) {
        for (const Row& r : rows) {
            if (r.option_id != option_id) continue;
            if (r.compact != nullptr) return r.compact;
            return r.element;
        }
        return nullptr;
    }

    Element* OptionsPage::add_row(const std::string& description) {
        ContextId context = get_current_context();
        Element* row = context.create_element<FocusRow>(list, text_shower(description));
        row->set_display(Display::Flex);
        row->set_flex_direction(FlexDirection::Row);
        row->set_align_items(AlignItems::Center);
        row->set_gap(16.0f);
        if (columns.empty()) {
            row->set_padding_left(12.0f);
            row->set_padding_right(12.0f);
        }
        else {
            // Buttons that don't fit beside each other in the column go on another line, keeping their full size.
            row->set_flex_wrap(FlexWrap::Wrap);
            row->set_padding_left(4.0f);
            row->set_padding_right(4.0f);
            column_items[column]++;
        }
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
            // Without an Apply button the change is final, so save it now: quitting with the menu open never runs
            // the tab's close callback, and settings read at launch (Graphics API) would come back unchanged.
            if (!cfg->requires_confirmation) {
                cfg->save_config();
            }
            if (apply_button != nullptr) {
                apply_button->set_enabled(any_dirty());
            }
        };
        on_option_hover_t on_hover = [this, cfg](const std::string& id) {
            show_description(cfg, id);
        };

        Row row{ cfg, option_id };
        if (!columns.empty()) {
            if (option.type == recomp::config::ConfigOptionType::None) {
                return;
            }
            row.compact = context.create_element<CompactOption>(list, cfg, option_id, set_value,
                [this, cfg, option_id]() { show_description(cfg, option_id); });
            column_items[column]++;
        }
        else {
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
            row.element = element;
        }
        rows.push_back(row);

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
        if (shown_config == nullptr && shown_default) {
            shown_default = false;  // Shows the new text.
            show_description(nullptr, "");
        }
    }

    void OptionsPage::show_description(recomp::config::Config* config, const std::string& option_id) {
        if (config == shown_config && option_id == shown_option && (config != nullptr || shown_default)) {
            return;  // Unchanged: setting the same text would lay the page out again for nothing.
        }
        shown_config = config;
        shown_option = option_id;
        shown_default = config == nullptr;
        description_text->set_text_unsafe(config != nullptr ? config->get_option(option_id).description : default_description);
    }

    void OptionsPage::show_text(const std::string& text) {
        if (!shown_default && shown_config == nullptr && shown_text == text) {
            return;
        }
        shown_config = nullptr;
        shown_option.clear();
        shown_default = false;
        shown_text = text;
        description_text->set_text_unsafe(text);
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
                if (row.config != config || row.option_id != id) {
                    continue;
                }
                for (UpdateType type : update.updates) {
                    if (type == UpdateType::Description) {
                        if (shown_config == config && shown_option == id) {
                            shown_option.clear();   // Shows the new text.
                            show_description(config, id);
                        }
                        continue;
                    }
                    if (row.compact != nullptr) {
                        switch (type) {
                            case UpdateType::Disabled: row.compact->update_disabled(); break;
                            case UpdateType::Hidden: row.compact->update_hidden(); break;
                            case UpdateType::EnumDetails: row.compact->update_enum_details(); break;
                            case UpdateType::EnumDisabled: row.compact->update_enum_disabled(); break;
                            case UpdateType::Value: row.compact->update_value(); break;
                            default: break;
                        }
                        continue;
                    }
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
                        default:
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
