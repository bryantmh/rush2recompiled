#ifndef __OPTIONS_PAGE_H__
#define __OPTIONS_PAGE_H__

#include <functional>
#include <string>
#include <vector>

#include "librecomp/config.hpp"
#include "elements/ui_config_page.h"
#include "elements/ui_button.h"
#include "elements/ui_modal.h"
#include "elements/ui_label.h"
#include "config/ui_config_option.h"

namespace rush2::ui {
    // A row of a scrolling list that scrolls itself into view when something in it takes focus, so the list follows
    // the d-pad down to buttons that are below the visible part. Use it (or OptionsPage::add_row) for any row of
    // buttons in a scrolling page: a plain div doesn't scroll, and the controller lands on buttons the player can't
    // see.
    class FocusRow : public recompui::Element {
    public:
        FocusRow(recompui::ResourceId rid, recompui::Element* parent);

    protected:
        std::string_view get_type_name() override { return "Rush2FocusRow"; }
        void process_event(const recompui::Event& e) override;
    };

    // A settings page built from option rows of any configs, in any order, under headings. Like the frontend's options
    // menu: the left side scrolls, the hovered or focused option's description shows on the right, and configs that
    // require confirmation get an Apply button.
    class OptionsPage : public recompui::ConfigPage {
    public:
        // description is shown on the right while no option is hovered.
        OptionsPage(recompui::ResourceId rid, recompui::Element* parent, const std::string& description = "");

        struct Heading {
            recompui::Element* row;    // Elements added to it (a button) go to the right of the title.
            recompui::Label* note;     // Dim text under the title, empty unless given.
        };
        Heading add_heading(const std::string& title, const std::string& note = "");
        // A row for buttons in the list, laid out left to right, that scrolls into view as they take focus.
        recompui::Element* add_row();
        void add_option(recomp::config::Config& config, const std::string& option_id);
        recompui::Element* get_list() { return list; }
        void set_default_description(const std::string& text);
        // Runs once per frame while the page is open (to show progress of work done in the background).
        void add_update_callback(std::function<void()> callback) { update_callbacks.push_back(std::move(callback)); }

    protected:
        std::string_view get_type_name() override { return "Rush2OptionsPage"; }
        void process_event(const recompui::Event& e) override;
        // Runs once per frame while the page is open.
        virtual void on_update() {}

    private:
        struct Row {
            recomp::config::Config* config;
            recompui::ConfigOptionElement* element;
        };
        recompui::Element* list = nullptr;
        recompui::Element* description_text = nullptr;
        recompui::Button* apply_button = nullptr;
        std::vector<Row> rows;
        int headings = 0;
        std::vector<recomp::config::Config*> configs;
        std::vector<std::function<void()>> update_callbacks;
        std::string default_description;
        std::string shown_option;
        recomp::config::Config* shown_config = nullptr;

        void show_description(recomp::config::Config* config, const std::string& option_id);
        void apply_updates(recomp::config::Config* config);
        bool any_dirty() const;
    };

    // Asks to apply or discard unapplied changes of a config that requires confirmation; a tab's can_close callback.
    bool confirm_close(const std::string& config_id, const std::string& name, recompui::TabCloseContext close_context);
}

#endif
