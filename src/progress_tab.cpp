// Progress tab: the Rush 2 keys and Dew cans, SF Rush keys and Rush 2049 coins each profile has found
// (src/collectibles.cpp), the unlock system's points and what each profile bought with them (src/unlocks.cpp), with a
// button that takes a profile's purchases back, and the Unlock System option. The page follows the game's save, the side save file and this session's no-profile
// players as they change.

#include <bit>
#include <set>
#include <string>
#include <vector>

#include "recompui/config.h"
#include "elements/ui_button.h"
#include "elements/ui_config_page.h"
#include "elements/ui_label.h"
#include "elements/ui_select.h"
#include "elements/ui_toggle.h"

#include "rush2.h"
#include "collectibles.h"
#include "unlocks.h"

namespace {
    using namespace recompui;
    using rush2::collectibles::Progress;

    const std::string tab_id = "progress";
    const std::string tab_name = "Progress";
    const std::string no_profile = "No profile (this session)";

    const std::string description =
        "Keys, Dew cans and coins found on every track, by player profile, and what they bought.\n\n"
        "Each Rush 2 track hides 12 keys and 4 Dew cans, SF Rush tracks hide keys as in San Francisco Rush, and Rush "
        "2049 tracks and stunt arenas hide 8 silver and 8 gold coins each. All of them are only out in one-player "
        "races, and once found they stay gone.\n\n"
        "With the Unlock System on, every key and silver coin is worth 1 point and every Dew can and gold coin 2. "
        "Spend them on cars, tracks and engines in the game's UNLOCKS menu (Start Game). With it off, each game's own "
        "rules unlock its cars: Rush 2's keys and cans on each track, SF Rush's keys, and Rush 2049's coin totals.\n\n"
        "Reset Purchases (press it twice) takes back everything the profile bought, returning its points to spend "
        "again; its keys and coins stay found.\n\n"
        "Players without a profile share their finds and purchases until the game is closed. Clearing or deleting a "
        "profile in the Records menu clears them.";

    // Rush 2's full track names (0x800C4C40).
    const char* const rush2_tracks[rush2::collectibles::rush2_courses] = { "Las Vegas", "Lower Manhattan", "Honolulu",
        "Upper Manhattan", "Alcatraz", "Los Angeles", "Seattle", "Halfpipe", "Crash", "Pipe", "Atari", "Stunt 1" };

    std::string label_of(const Progress& p) {
        if (p.name.empty()) {
            return no_profile;
        }
        std::string s;
        for (char c : p.name) {
            s.push_back(c >= 0x20 && c < 0x7F ? c : '?');
        }
        return s;
    }

    std::string state_of(const std::vector<Progress>& list) {
        std::string s = rush2::unlocks::enabled() ? "on;" : "off;";
        for (const Progress& p : list) {
            s += label_of(p) + ":" + std::to_string(rush2::collectibles::points(p.name)) + ":";
            for (const std::string& id : rush2::collectibles::purchases(p.name)) s += id + ",";
            for (uint16_t m : p.rush2) s += std::to_string(m) + ",";
            for (uint16_t m : p.sfrush) s += std::to_string(m) + ",";
            for (uint16_t m : p.rush2049) s += std::to_string(m) + ",";
            for (uint16_t m : p.stunt2049) s += std::to_string(m) + ",";
            s += ";";
        }
        return s;
    }

    std::string of(int found, int all) {
        return std::to_string(found) + "/" + std::to_string(all);
    }

    // A game's section. It takes focus, so a controller can move through the sections and scroll the list.
    class Section : public Element {
    public:
        Section(ResourceId rid, Element* parent) : Element(rid, parent, Events(EventType::Focus, EventType::Hover), "div", false) {
            enable_focus();
            set_display(Display::Block);
            set_padding(12.0f);
            set_margin_bottom(4.0f);
            set_border_radius(theme::border::radius_sm);
            set_background_color(theme::color::Transparent);
            focus_style.set_background_color(theme::color::Elevated);
            add_style(&focus_style, focus_state);
        }

    protected:
        std::string_view get_type_name() override { return "ProgressSection"; }

        void process_event(const Event& e) override {
            if (e.type == EventType::Focus) {
                bool active = std::get<EventFocus>(e.variant).active;
                set_style_enabled(focus_state, active);
                if (active) {
                    scroll_into_view();
                }
            }
        }

    private:
        Style focus_style;
    };

    // A row above the sections (the Unlock System option, the profile picker): scrolls itself into view when something
    // in it takes focus, so moving back up with the d-pad scrolls the list back to the top.
    class Row : public Element {
    public:
        Row(ResourceId rid, Element* parent) : Element(rid, parent, Events(EventType::Focus), "div", false) {}

    protected:
        std::string_view get_type_name() override { return "ProgressRow"; }

        void process_event(const Event& e) override {
            if (e.type == EventType::Focus && std::get<EventFocus>(e.variant).active) {
                scroll_into_view();
            }
        }
    };

    class ProgressPage : public ConfigPage {
    public:
        ProgressPage(ResourceId rid, Element* parent) : ConfigPage(rid, parent, Events(EventType::Update)) {
            ContextId context = get_current_context();
            set_as_navigation_container(NavigationType::Vertical);

            Element* left = body->get_left();
            left->set_padding(0.0f);
            left->set_display(Display::Block);
            left->set_position(Position::Relative);
            left->set_height(100.0f, Unit::Percent);
            Element* column = context.create_element<Element>(left, 0, "div", false);
            column->set_display(Display::Block);
            column->set_width(100.0f, Unit::Percent);
            column->set_min_height(100.0f, Unit::Percent);
            column->set_max_height(100.0f, Unit::Percent);
            column->set_padding(16.0f);
            column->set_overflow_y(Overflow::Auto);
            column->set_as_navigation_container(NavigationType::Vertical);
            Element* option = context.create_element<Row>(column);
            option->set_display(Display::Flex);
            option->set_flex_direction(FlexDirection::Row);
            option->set_align_items(AlignItems::Center);
            option->set_padding(12.0f);
            option->set_gap(16.0f);
            option->set_as_navigation_container(NavigationType::Horizontal);
            Label* option_name = context.create_element<Label>(option, "Unlock System", theme::Typography::LabelMD);
            option_name->set_flex_grow(1.0f);
            system_toggle = context.create_element<Toggle>(option, ToggleSize::Medium);
            system_toggle->set_checked(rush2::unlocks::enabled());
            system_toggle->add_checked_callback([this](bool checked) {
                rush2::cheats::set_unlock_system(checked);
                shown_state.clear();
            });
            picker = context.create_element<Row>(column);
            picker->set_display(Display::Flex);
            picker->set_flex_direction(FlexDirection::Row);
            picker->set_align_items(AlignItems::Center);
            picker->set_padding(12.0f);
            picker->set_gap(16.0f);
            picker->set_as_navigation_container(NavigationType::Horizontal);
            content = context.create_element<Element>(column, 0, "div", false);
            content->set_display(Display::Block);
            content->set_width(100.0f, Unit::Percent);
            content->set_as_navigation_container(NavigationType::Vertical);

            Element* text = context.create_element<Element>(body->get_right(), 0, "p", true);
            text->set_typography(theme::Typography::Body);
            text->set_line_height(28.0f);
            text->set_padding(8.0f);
            std::string html;
            for (char c : description) {
                html += c == '\n' ? std::string("<br/>") : std::string(1, c);
            }
            text->set_text_unsafe(html);

            refresh();
            queue_update();
        }

    protected:
        std::string_view get_type_name() override { return "ProgressPage"; }

        void process_event(const Event& e) override {
            if (e.type == EventType::Update) {
                refresh();
                queue_update();
            }
        }

    private:
        // One track's line: its name, then a count per kind of find (dim until all are found).
        struct Line {
            std::string name;
            std::vector<std::pair<int, int>> counts;    // Found, all.
        };

        Element* picker = nullptr;
        Toggle* system_toggle = nullptr;
        Element* content = nullptr;
        std::vector<Progress> list;
        std::string shown_names;
        std::string shown_state;
        std::string selected = no_profile;
        bool picked = false;    // The profile was chosen in the picker; until then the first one is shown.
        bool reset_armed = false;   // Reset Purchases was pressed once.

        void refresh() {
            std::vector<Progress> now = rush2::collectibles::progress();
            std::string state = state_of(now) + "|" + selected;
            if (state == shown_state) {
                return;
            }
            if (system_toggle != nullptr && system_toggle->is_checked() != rush2::unlocks::enabled()) {
                system_toggle->set_checked(rush2::unlocks::enabled());
            }
            list = std::move(now);
            shown_state = state;
            std::string names;
            bool found = false;
            for (const Progress& p : list) {
                names += label_of(p) + ";";
                found = found || label_of(p) == selected;
            }
            if (!found || !picked) {
                selected = list.empty() ? no_profile : label_of(list.front());
            }
            // Rebuild the picker only when the profiles change (rebuilding it would drop its focus).
            if (names != shown_names) {
                shown_names = names;
                build_picker();
            }
            build_content();
        }

        void build_picker() {
            ContextId context = get_current_context();
            picker->clear_children();
            context.create_element<Label>(picker, "Profile", theme::Typography::LabelMD);
            std::vector<SelectOption> options;
            for (const Progress& p : list) {
                options.emplace_back(label_of(p), label_of(p));
            }
            Select* select = context.create_element<Select>(picker, options, selected);
            select->add_change_callback([this](SelectOption& option, int) {
                selected = option.value;
                picked = true;
                reset_armed = false;
                shown_state.clear();
            });
        }

        // A section: the game's title and totals, then its tracks in two columns (the second starting at line split,
        // or halfway).
        void add_section(const std::string& title, const std::vector<std::string>& kinds, const std::vector<Line>& lines,
                         size_t split = 0) {
            ContextId context = get_current_context();
            Section* section = context.create_element<Section>(content);

            std::vector<std::pair<int, int>> totals(kinds.size());
            for (const Line& line : lines) {
                for (size_t k = 0; k < kinds.size(); k++) {
                    totals[k].first += line.counts[k].first;
                    totals[k].second += line.counts[k].second;
                }
            }
            Element* head = context.create_element<Element>(section, 0, "div", false);
            head->set_display(Display::Flex);
            head->set_flex_direction(FlexDirection::Row);
            head->set_align_items(AlignItems::Center);
            head->set_gap(16.0f);
            head->set_padding_bottom(6.0f);
            head->set_margin_bottom(8.0f);
            head->set_border_bottom_width(1.0f);
            head->set_border_bottom_color(theme::color::Border);
            Label* name = context.create_element<Label>(head, title, theme::Typography::LabelLG);
            name->set_flex_grow(1.0f);
            name->set_white_space(WhiteSpace::Nowrap);
            for (size_t k = 0; k < kinds.size(); k++) {
                Label* total = context.create_element<Label>(head, of(totals[k].first, totals[k].second) + " " + kinds[k],
                                                             theme::Typography::LabelMD);
                total->set_white_space(WhiteSpace::Nowrap);
            }

            Element* grid = context.create_element<Element>(section, 0, "div", false);
            grid->set_display(Display::Flex);
            grid->set_flex_direction(FlexDirection::Row);
            grid->set_gap(32.0f);
            size_t half = split != 0 ? split : (lines.size() + 1) / 2;
            for (size_t c = 0; c < 2; c++) {
                Element* column = context.create_element<Element>(grid, 0, "div", false);
                column->set_display(Display::Block);
                column->set_flex_grow(1.0f);
                column->set_flex_basis(0.0f);
                for (size_t i = c == 0 ? 0 : half; i < (c == 0 ? half : lines.size()); i++) {
                    const Line& line = lines[i];
                    Element* row = context.create_element<Element>(column, 0, "div", false);
                    row->set_display(Display::Flex);
                    row->set_flex_direction(FlexDirection::Row);
                    row->set_gap(12.0f);
                    row->set_margin_bottom(2.0f);
                    Label* track = context.create_element<Label>(row, line.name, theme::Typography::Body);
                    track->set_flex_grow(1.0f);
                    for (const auto& [found, all] : line.counts) {
                        Label* count = context.create_element<Label>(row, of(found, all), theme::Typography::Body);
                        count->set_min_width(44.0f);
                        count->set_text_align(TextAlign::Right);
                        count->set_color(found == all ? theme::color::Text : theme::color::TextDim);
                    }
                }
            }
        }

        // The unlock system: points earned and left, then every item with its cost, bought ones bright.
        void add_unlocks(const Progress& p) {
            ContextId context = get_current_context();
            Section* section = context.create_element<Section>(content);
            std::set<std::string> bought = rush2::collectibles::purchases(p.name);
            int points = rush2::collectibles::points(p.name);
            int spent = rush2::unlocks::spent(bought);

            Element* head = context.create_element<Element>(section, 0, "div", false);
            head->set_display(Display::Flex);
            head->set_flex_direction(FlexDirection::Row);
            head->set_align_items(AlignItems::Center);
            head->set_gap(16.0f);
            head->set_padding_bottom(6.0f);
            head->set_margin_bottom(8.0f);
            head->set_border_bottom_width(1.0f);
            head->set_border_bottom_color(theme::color::Border);
            Label* name = context.create_element<Label>(head, "Unlocks", theme::Typography::LabelLG);
            name->set_flex_grow(1.0f);
            Label* found = context.create_element<Label>(head, std::to_string(points) + "/" +
                std::to_string(rush2::collectibles::max_points()) + " points found", theme::Typography::LabelMD);
            found->set_white_space(WhiteSpace::Nowrap);
            Label* left = context.create_element<Label>(head, std::to_string(points - spent) + " to spend",
                                                        theme::Typography::LabelMD);
            left->set_white_space(WhiteSpace::Nowrap);

            Element* grid = context.create_element<Element>(section, 0, "div", false);
            grid->set_display(Display::Flex);
            grid->set_flex_direction(FlexDirection::Row);
            grid->set_gap(32.0f);
            const rush2::unlocks::Kind columns[2][2] = { { rush2::unlocks::Kind::Car, rush2::unlocks::Kind::Car },
                                                          { rush2::unlocks::Kind::Track, rush2::unlocks::Kind::Part } };
            for (const auto& kinds : columns) {
                Element* column = context.create_element<Element>(grid, 0, "div", false);
                column->set_display(Display::Block);
                column->set_flex_grow(1.0f);
                column->set_flex_basis(0.0f);
                for (const auto& item : rush2::unlocks::items()) {
                    bool in_column = item.kind == kinds[0] || item.kind == kinds[1];
                    if (!in_column || !rush2::unlocks::item_available(item)) continue;
                    bool owned = bought.contains(item.id);
                    Element* row = context.create_element<Element>(column, 0, "div", false);
                    row->set_display(Display::Flex);
                    row->set_flex_direction(FlexDirection::Row);
                    row->set_gap(12.0f);
                    row->set_margin_bottom(2.0f);
                    std::string title = std::string(item.name) + " (" + item.game + ")";
                    Label* label = context.create_element<Label>(row, title, theme::Typography::Body);
                    label->set_flex_grow(1.0f);
                    label->set_color(owned ? theme::color::Text : theme::color::TextDim);
                    Label* cost = context.create_element<Label>(row, owned ? "Owned" : std::to_string(item.cost) + " pts",
                                                                theme::Typography::Body);
                    cost->set_min_width(64.0f);
                    cost->set_text_align(TextAlign::Right);
                    cost->set_color(owned ? theme::color::Text : theme::color::TextDim);
                }
            }
            Element* footer = context.create_element<Element>(section, 0, "div", false);
            footer->set_display(Display::Flex);
            footer->set_flex_direction(FlexDirection::Row);
            footer->set_justify_content(JustifyContent::FlexEnd);
            footer->set_margin_top(8.0f);
            Button* reset = context.create_element<Button>(footer, reset_armed ? "Press Again to Reset" : "Reset Purchases",
                                                           ButtonStyle::Secondary);
            reset->set_enabled(!bought.empty());
            std::string profile = p.name;
            reset->add_pressed_callback([this, reset, profile]() {
                if (!reset_armed) {
                    reset_armed = true;
                    reset->set_text("Press Again to Reset");
                    return;
                }
                reset_armed = false;
                rush2::collectibles::reset_purchases(profile);
                shown_state.clear();
            });
        }

        void build_content() {
            content->clear_children();
            const Progress* p = nullptr;
            for (const Progress& q : list) {
                if (label_of(q) == selected) {
                    p = &q;
                }
            }
            if (p == nullptr) {
                return;
            }
            using namespace rush2::collectibles;
            auto count = [](uint16_t m) { return std::popcount((unsigned)m); };
            std::vector<Line> lines;
            for (int t = 0; t < rush2_courses; t++) {
                lines.push_back({ rush2_tracks[t], {
                    { count(p->rush2[t] & rush2_key_bits), rush2::collectibles::rush2_keys },
                    { count(p->rush2[t] & rush2_can_bits), rush2_cans } } });
            }
            add_section("Rush 2", { "keys", "Dew cans" }, lines);

            lines.clear();
            for (int t = 0; t < sfrush_courses; t++) {
                lines.push_back({ "Track " + std::to_string(t + 1), { { count(p->sfrush[t]), sfrush_keys[t] } } });
            }
            add_section("SF Rush", { "keys" }, lines);

            // Race tracks in the left column, stunt arenas in the right.
            lines.clear();
            auto coins = [&](const std::string& course, const uint16_t* masks, int n) {
                for (int t = 0; t < n; t++) {
                    lines.push_back({ course + " " + std::to_string(t + 1), {
                        { count(masks[t] & silver_bits), coins_per_kind },
                        { count(masks[t] & gold_bits), coins_per_kind } } });
                }
            };
            coins("Track", p->rush2049.data(), rush2049_courses);
            coins("Arena", p->stunt2049.data(), stunt2049_courses);
            add_section("Rush 2049", { "silver", "gold" }, lines, rush2049_courses);

            if (rush2::unlocks::enabled()) {
                add_unlocks(*p);
            }
        }
    };
}

void rush2::collectibles::create_tab() {
    recompui::config::create_tab(tab_name, tab_id, [](ContextId context, Element* parent) {
        context.create_element<ProgressPage>(parent);
    });
}
