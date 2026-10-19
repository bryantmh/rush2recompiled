#ifndef __OPTIONS_PAGE_H__
#define __OPTIONS_PAGE_H__

#include <functional>
#include <memory>
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
    // see. on_show, if given, runs when the row or something in it is hovered or takes focus: pages use it to show the
    // row's description (explanations go there, not in the row).
    class FocusRow : public recompui::Element {
    public:
        FocusRow(recompui::ResourceId rid, recompui::Element* parent, std::function<void()> on_show = nullptr);

    protected:
        std::string_view get_type_name() override { return "Rush2FocusRow"; }
        void process_event(const recompui::Event& e) override;

    private:
        std::function<void()> on_show;
    };

    // Where sections with a Show/Hide list (AI Opponents, Circuit Courses) open their list: a spare column of a column
    // page, so opening one never makes the column with its button grow, scroll or move. One list shows at a time:
    // a section opening its list calls the others' closers.
    struct ExpandPanel {
        recompui::Element* area = nullptr;
        std::vector<std::function<void()>> closers;

        // Adds a section's closer, returning its index for open().
        size_t add(std::function<void()> closer) {
            closers.push_back(std::move(closer));
            return closers.size() - 1;
        }
        // Closes every list but the opener's.
        void open(size_t opener) {
            for (size_t i = 0; i < closers.size(); i++) {
                if (i != opener) closers[i]();
            }
        }
    };

    class CompactOption;

    // A settings page built from option rows of any configs, in any order, under headings. Like the frontend's options
    // menu: the left side scrolls, the hovered or focused option's description shows on the right, and configs that
    // require confirmation get an Apply button.
    //
    // With columns > 0 the page is laid out like the Cheats tab instead: side-by-side columns that each scroll on their
    // own, with the description across the bottom. Each option is one line, its name and then its control (a switch,
    // a dropdown of its choices, a slider or a text box). Headings, rows and options go into the current column;
    // next_column() moves on to the next one. The columns are block boxes, not flex ones: RmlUi lays out every
    // nested flex box several times over, and a page of rows in flex columns took tens of milliseconds to lay out
    // (tools/rml_layout_bench).
    class OptionsPage : public recompui::ConfigPage {
    public:
        // description is shown on the right (or below the columns) while no option is hovered.
        OptionsPage(recompui::ResourceId rid, recompui::Element* parent, const std::string& description = "", int columns = 0);

        struct Heading {
            recompui::Element* row;    // Elements added to it (a button) go to the right of the title.
            recompui::Label* note;     // Dim text under the title, empty unless given (hidden while empty).
            recompui::Label* title;
        };
        // note is short status text under the title; description shows on the right while the heading is hovered.
        Heading add_heading(const std::string& title, const std::string& note = "", const std::string& description = "");
        // A row for buttons in the list, laid out left to right, that scrolls into view as they take focus. description
        // shows on the right while the row is hovered or focused.
        recompui::Element* add_row(const std::string& description = "");
        void add_option(recomp::config::Config& config, const std::string& option_id);
        // Dim status text in the top right corner of an added option's row (what it's doing in the background), laid
        // over the row so showing or clearing it moves nothing. Null if the option isn't on the page.
        recompui::Label* add_option_status(const std::string& option_id);
        // An added option's row (to show or hide it), or null.
        recompui::Element* get_option_row(const std::string& option_id);
        // Where headings, rows and options go: the list, or the current column.
        recompui::Element* get_list() { return list; }
        // Column layout: what's added next goes at the top of the next column (the last column keeps it).
        void next_column();
        // Column layout: the page's last column as the place sections open their Show/Hide lists (leave it empty
        // otherwise). The same panel every call.
        std::shared_ptr<ExpandPanel> expand_panel();
        void set_default_description(const std::string& text);
        // Runs once per frame while the page is open (to show progress of work done in the background).
        void add_update_callback(std::function<void()> callback) { update_callbacks.push_back(std::move(callback)); }
        // A callback that shows text in the description pane (for a FocusRow's on_show); null for empty text.
        std::function<void()> text_shower(const std::string& text);

    protected:
        std::string_view get_type_name() override { return "Rush2OptionsPage"; }
        void process_event(const recompui::Event& e) override;
        // Runs once per frame while the page is open.
        virtual void on_update() {}

    private:
        // One of element (list layout) and compact (column layout) is set.
        struct Row {
            recomp::config::Config* config;
            std::string option_id;
            recompui::ConfigOptionElement* element = nullptr;
            CompactOption* compact = nullptr;
        };
        recompui::Element* list = nullptr;
        std::vector<recompui::Element*> columns;
        std::shared_ptr<ExpandPanel> panel;
        size_t column = 0;
        // What has gone into each column, for the top spacing of its first heading.
        std::vector<int> column_items;
        recompui::Element* description_text = nullptr;
        recompui::Button* apply_button = nullptr;
        std::vector<Row> rows;
        int headings = 0;
        std::vector<recomp::config::Config*> configs;
        std::vector<std::function<void()>> update_callbacks;
        std::string default_description;
        std::string shown_option;
        recomp::config::Config* shown_config = nullptr;
        // What the description shows, so showing the same again doesn't change its text (which lays the page out).
        bool shown_default = false;
        std::string shown_text;

        void show_description(recomp::config::Config* config, const std::string& option_id);
        void show_text(const std::string& text);
        void apply_updates(recomp::config::Config* config);
        bool any_dirty() const;
    };

    // Asks to apply or discard unapplied changes of a config that requires confirmation; a tab's can_close callback.
    bool confirm_close(const std::string& config_id, const std::string& name, recompui::TabCloseContext close_context);
}

#endif
