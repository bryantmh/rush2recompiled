// Progress tab: the Rush 2 keys and Dew cans, SF Rush keys and Rush 2049 coins each profile has found
// (src/collectibles.cpp). Read only; the page follows the game's save, collectibles.json and this session's
// no-profile players as they change.

#include <bit>
#include <string>
#include <vector>

#include "recompui/config.h"
#include "elements/ui_config_page.h"
#include "elements/ui_label.h"
#include "elements/ui_select.h"

#include "collectibles.h"

namespace {
    using namespace recompui;
    using rush2::collectibles::Progress;

    const std::string tab_id = "progress";
    const std::string tab_name = "Progress";
    const std::string no_profile = "No profile (this session)";

    const std::string description =
        "Keys, Dew cans and coins found on every track, by player profile.\n\n"
        "Each Rush 2 track hides 12 keys and 4 Dew cans: every 3 keys found on a track unlock one of its 4 mystery cars "
        "there, and all 4 cans its Dew car. SF Rush tracks hide keys as in San Francisco Rush, and Rush 2049 tracks and "
        "stunt arenas hide 8 silver and 8 gold coins each. All of them are only out in one-player races, and once "
        "found they stay gone.\n\n"
        "Players without a profile share their finds until the game is closed. Clearing or deleting a profile in the "
        "Records menu clears its finds.\n\n"
        "The SF Rush keys and Rush 2049 coins don't unlock anything yet.";

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
        std::string s;
        for (const Progress& p : list) {
            s += label_of(p) + ":";
            for (uint16_t m : p.rush2) s += std::to_string(m) + ",";
            for (uint16_t m : p.sfrush) s += std::to_string(m) + ",";
            for (uint16_t m : p.rush2049) s += std::to_string(m) + ",";
            for (uint16_t m : p.stunt2049) s += std::to_string(m) + ",";
            s += ";";
        }
        return s;
    }

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
            picker = context.create_element<Element>(column, 0, "div", false);
            picker->set_display(Display::Flex);
            picker->set_flex_direction(FlexDirection::Column);
            picker->set_padding(12.0f);
            picker->set_gap(8.0f);
            picker->set_as_navigation_container(NavigationType::Vertical);
            content = context.create_element<Element>(column, 0, "div", false);
            content->set_display(Display::Flex);
            content->set_flex_direction(FlexDirection::Column);
            content->set_width(100.0f, Unit::Percent);

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
        Element* picker = nullptr;
        Element* content = nullptr;
        std::vector<Progress> list;
        std::string shown_names;
        std::string shown_state;
        std::string selected = no_profile;
        bool picked = false;    // The profile was chosen in the picker; until then the first one is shown.

        void refresh() {
            std::vector<Progress> now = rush2::collectibles::progress();
            std::string state = state_of(now) + "|" + selected;
            if (state == shown_state) {
                return;
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
                shown_state.clear();
            });
        }

        void add_section(const std::string& title, const std::vector<std::string>& lines, const std::string& total) {
            ContextId context = get_current_context();
            Element* section = context.create_element<Element>(content, 0, "div", false);
            section->set_display(Display::Flex);
            section->set_flex_direction(FlexDirection::Column);
            section->set_padding(12.0f);
            section->set_gap(4.0f);
            context.create_element<Label>(section, title, theme::Typography::LabelMD);
            for (const std::string& line : lines) {
                context.create_element<Label>(section, line, theme::Typography::Body);
            }
            Label* sum = context.create_element<Label>(section, total, theme::Typography::Body);
            sum->set_color(theme::color::TextDim);
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

            std::vector<std::string> rush2_lines;
            int keys = 0, cans = 0;
            for (int t = 0; t < rush2_courses; t++) {
                int k = count(p->rush2[t] & rush2_key_bits), c = count(p->rush2[t] & rush2_can_bits);
                rush2_lines.push_back(std::string(rush2_tracks[t]) + ": " + std::to_string(k) + " of " +
                                      std::to_string(rush2::collectibles::rush2_keys) + " keys, " + std::to_string(c) +
                                      " of " + std::to_string(rush2_cans) + " Dew cans");
                keys += k;
                cans += c;
            }
            add_section("Rush 2 Keys and Dew Cans", rush2_lines,
                        "All tracks: " + std::to_string(keys) + " of " +
                        std::to_string(rush2_courses * rush2::collectibles::rush2_keys) + " keys, " +
                        std::to_string(cans) + " of " + std::to_string(rush2_courses * rush2_cans) + " Dew cans");

            std::vector<std::string> lines;
            int found = 0, all = 0;
            for (int t = 0; t < sfrush_courses; t++) {
                int n = count(p->sfrush[t]);
                lines.push_back("Track " + std::to_string(t + 1) + ": " + std::to_string(n) + " of " +
                                std::to_string(sfrush_keys[t]) + " keys");
                found += n;
                all += sfrush_keys[t];
            }
            add_section("SF Rush Keys", lines, "All tracks: " + std::to_string(found) + " of " + std::to_string(all));

            auto coins = [&](const std::string& title, const std::string& course, const uint16_t* masks, int n) {
                std::vector<std::string> coin_lines;
                int silver = 0, gold = 0;
                for (int t = 0; t < n; t++) {
                    int s = count(masks[t] & silver_bits), g = count(masks[t] & gold_bits);
                    coin_lines.push_back(course + " " + std::to_string(t + 1) + ": " + std::to_string(s) + " of " +
                                         std::to_string(coins_per_kind) + " silver, " + std::to_string(g) + " of " +
                                         std::to_string(coins_per_kind) + " gold");
                    silver += s;
                    gold += g;
                }
                std::string of = std::to_string(n * coins_per_kind);
                add_section(title, coin_lines, "All: " + std::to_string(silver) + " of " + of + " silver, " +
                                               std::to_string(gold) + " of " + of + " gold");
            };
            coins("Rush 2049 Coins", "Track", p->rush2049.data(), rush2049_courses);
            coins("Rush 2049 Stunt Arena Coins", "Arena", p->stunt2049.data(), stunt2049_courses);
        }
    };
}

void rush2::collectibles::create_tab() {
    recompui::config::create_tab(tab_name, tab_id, [](ContextId context, Element* parent) {
        context.create_element<ProgressPage>(parent);
    });
}
