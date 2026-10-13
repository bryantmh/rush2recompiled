// Circuit length and courses (General tab: the Circuit Length option and the Circuit Courses section).
//
// A Rush 2 circuit is 28 races (docs/rush2049_research/menus.md, section 9). The profile's circuit block (0x60 bytes at
// player record +0x464 + humans * 0x60, pointer in 0x800D3E40) holds +1 flags (bit 0: a circuit is in progress),
// +2 the races finished and +4 the random seed the races are generated from. func_800B2A58 starts a new circuit: it
// clears the block, takes a seed from the timer and sets bit 0. The length 28 is compared in six places, each hooked
// to compare with the circuit's length L instead:
// - func_800A7DCC at 0x800A7E5C: the current race 0x800D3DF0 = min(finished, 27) -> min(finished, L - 1).
// - func_800A8D1C at 0x800A8F54: finished >= 28 completes the circuit (func_800A8B04, stats and the victory screen).
// - func_800A952C at 0x800A9670: the race just run was not the last (index < 27) -> the settings for the next race.
// - func_803B6CA8 at 0x803B6CB8: NEXT RACE is offered while finished < 28 (0x803C7894).
// - func_803B7018 at 0x803B7158: back from a circuit race with finished >= 28 -> the results (state 0xC).
// - func_803BCFA4 at 0x803BCFE0: the circuit screen's races left, 28 - finished ($s3 = 28).
// The places of each race are kept in 3 bits a race (11 bytes a car), room for 29; the standings loops already stop at
// the races finished. Rush 2's circuit codes (func_800B2BCC) still hold 28-race circuits.
//
// The courses a circuit picks from are chosen per track (Rush 2's 0-6, Rush 2049's 12-17, SF Rush's 18-24); the
// generator hook (rush2_track49_circuit, src/rush2049/track2049_menu.cpp) draws from the chosen ones that are
// available. A circuit keeps the length and courses it started with: the start (end of func_800B2A58) records them
// against its seed in the saves file's "circuits" section. A circuit with no record (started before this setting
// existed, or entered as a code) is 28 races over every course, as before.

// librecomp's own nlohmann::json first, so Config::load_config links (see src/rush2049/wings.cpp).
#include "../lib/N64ModernRuntime/thirdparty/json/json.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <deque>
#include <mutex>
#include <string>
#include <vector>

#include "recomp.h"
#include "librecomp/config.hpp"
#include "recompui/config.h"
#include "elements/ui_button.h"
#include "elements/ui_label.h"
#include "elements/ui_select.h"
#include "elements/ui_toggle.h"

#include "circuit.h"
#include "data_files.h"
#include "options_page.h"
#include "rush2_hooks.h"
#include "track1.h"
#include "track2049.h"

namespace {
    using rush2::circuit::course_ids;
    using rush2::circuit::stock_length;

    constexpr uint32_t circuit_block = 0x800D3E40;  // void*: the current profile's circuit block
    constexpr int32_t block_finished = 0x2;         // s8: races finished (offsets signed: MEM_ macros)
    constexpr int32_t block_seed = 0x4;             // u32

    std::atomic<int> next_length = stock_length;
    std::array<std::atomic<bool>, course_ids> next_courses;

    // Track select ids a circuit can pick, by game.
    struct Game {
        const char* name;
        int first;
        int count;
    };
    constexpr Game games[] = {
        { "Rush 2", 0, 7 },
        { "Rush 2049", rush2::track2049::first_menu_id, rush2::track2049::track_count },
        { "SF Rush", rush2::track1::first_menu_id, rush2::track1::track_count },
    };
    // Rush 2's full track names (0x800C4C40).
    const char* const rush2_names[7] = { "Las Vegas", "Lower Manhattan", "Honolulu", "Upper Manhattan", "Alcatraz",
                                         "Los Angeles", "Seattle" };

    bool is_course(int t) {
        for (const Game& g : games) {
            if (t >= g.first && t < g.first + g.count) return true;
        }
        return false;
    }

    std::string course_name(int t) {
        if (t < 7) return rush2_names[t];
        if (t < rush2::track1::first_menu_id) return rush2::track2049::track_names[t - rush2::track2049::first_menu_id];
        return rush2::track1::track_names[t - rush2::track1::first_menu_id];
    }

    // What a circuit was started with.
    struct Setup {
        uint32_t seed = 0;
        int length = stock_length;
        uint32_t courses = 0xFFFFFFFF; // bit t: track t
    };

    constexpr const char* saves_section = "circuits";
    constexpr size_t setups_kept = 64; // The oldest go first (a profile's circuit block only holds its latest).
    std::mutex setups_mutex;
    std::deque<Setup> setups;
    bool setups_loaded = false;

    void load_setups() {
        if (setups_loaded) return;
        setups_loaded = true;
        std::string text = rush2::data_files::read(rush2::data_files::File::Saves, saves_section);
        if (text.empty()) return;
        nlohmann::json j = nlohmann::json::parse(text, nullptr, false);
        if (!j.is_object() || !j["list"].is_array()) return;
        for (const auto& e : j["list"]) {
            if (!e.is_object() || !e["seed"].is_number_unsigned() || !e["races"].is_number_integer() ||
                !e["courses"].is_array()) {
                continue;
            }
            Setup s;
            s.seed = e["seed"].get<uint32_t>();
            s.length = std::clamp(e["races"].get<int>(), 1, stock_length);
            s.courses = 0;
            for (const auto& c : e["courses"]) {
                if (c.is_number_integer() && is_course(c.get<int>())) s.courses |= 1u << c.get<int>();
            }
            setups.push_back(s);
        }
    }

    void save_setups() {
        nlohmann::json list = nlohmann::json::array();
        for (const Setup& s : setups) {
            nlohmann::json courses = nlohmann::json::array();
            for (int t = 0; t < course_ids; t++) {
                if (s.courses & (1u << t)) courses.push_back(t);
            }
            list.push_back({ { "seed", s.seed }, { "races", s.length }, { "courses", courses } });
        }
        rush2::data_files::write(rush2::data_files::File::Saves, saves_section,
            nlohmann::json{ { "version", 1 }, { "list", list } }.dump());
    }

    // The current circuit's setup (Rush 2's when it has no record).
    Setup current_setup(uint8_t* rdram) {
        uint32_t block = (uint32_t)MEM_W(0, (int32_t)circuit_block);
        Setup found;
        if (block == 0) return found;
        uint32_t seed = (uint32_t)MEM_W(block_seed, (int32_t)block);
        std::lock_guard lock{ setups_mutex };
        load_setups();
        for (const Setup& s : setups) {
            if (s.seed == seed) found = s;
        }
        return found;
    }

    int finished(uint8_t* rdram) {
        uint32_t block = (uint32_t)MEM_W(0, (int32_t)circuit_block);
        return block == 0 ? 0 : (int8_t)MEM_B(block_finished, (int32_t)block);
    }

    // The choices for the next circuit (circuit.json, hidden options: races, track_<id>).
    const std::string config_id = "circuit";
    recomp::config::Config circuit_config{ "Circuit", config_id, false };

    std::string option_id(int t) {
        return "track_" + std::to_string(t);
    }

    const std::string length_id = "races";

    void set_course(int t, bool on) {
        next_courses[t].store(on);
        circuit_config.update_option_value(option_id(t), on);
    }
}

int rush2::circuit::length(uint8_t* rdram) {
    return current_setup(rdram).length;
}

bool rush2::circuit::course_chosen(uint8_t* rdram, int t) {
    return t >= 0 && t < course_ids && (current_setup(rdram).courses & (1u << t)) != 0;
}

void rush2::circuit::init_config() {
    circuit_config.add_number_option(length_id, "Circuit Length", "", 1, stock_length, 1, 0, false, stock_length, true);
    for (int t = 0; t < course_ids; t++) {
        next_courses[t].store(true);
        if (is_course(t)) {
            circuit_config.add_bool_option(option_id(t), course_name(t), "", true, true);
        }
    }
}

void rush2::circuit::load_config() {
    circuit_config.load_config();
    next_length.store(std::clamp((int)std::get<double>(circuit_config.get_option_value(length_id)), 1, stock_length));
    for (int t = 0; t < course_ids; t++) {
        if (is_course(t)) {
            next_courses[t].store(std::get<bool>(circuit_config.get_option_value(option_id(t))));
        }
    }
}

// End of func_800B2A58 (a new circuit; also run before a circuit code is entered, which then replaces the seed): the
// circuit keeps the length and courses chosen now.
extern "C" void rush2_circuit_started(uint8_t* rdram, recomp_context* ctx) {
    uint32_t block = (uint32_t)MEM_W(0, (int32_t)circuit_block);
    if (block == 0) return;
    Setup s;
    s.seed = (uint32_t)MEM_W(block_seed, (int32_t)block);
    s.length = next_length.load();
    s.courses = 0;
    for (int t = 0; t < course_ids; t++) {
        if (is_course(t) && next_courses[t].load()) s.courses |= 1u << t;
    }
    std::lock_guard lock{ setups_mutex };
    load_setups();
    std::erase_if(setups, [&](const Setup& old) { return old.seed == s.seed; });
    setups.push_back(s);
    while (setups.size() > setups_kept) setups.pop_front();
    save_setups();
    printf("[circuit] New circuit (seed %05X): %d races\n", s.seed, s.length);
    std::fflush(stdout);
}

// func_800A7DCC at 0x800A7E5C: $v0 = races finished, about to give the current race min($v0, 27).
extern "C" void rush2_circuit_race_index(uint8_t* rdram, recomp_context* ctx) {
    int last = rush2::circuit::length(rdram) - 1;
    if ((int32_t)ctx->r2 > last) ctx->r2 = (uint64_t)(int64_t)last;
}

// func_800A8D1C at 0x800A8F54 ($t7) and func_803B7018 at 0x803B7158 ($t9): races finished, about to be compared with
// 28 for a complete circuit. func_803B6CA8 at 0x803B6CB8 ($v0): the same, for NEXT RACE.
static void complete_at_length(uint8_t* rdram, uint64_t& finished_reg) {
    if ((int32_t)finished_reg >= rush2::circuit::length(rdram)) finished_reg = stock_length;
}

extern "C" void rush2_circuit_complete_t7(uint8_t* rdram, recomp_context* ctx) {
    complete_at_length(rdram, ctx->r15);
}

extern "C" void rush2_circuit_complete_t9(uint8_t* rdram, recomp_context* ctx) {
    complete_at_length(rdram, ctx->r25);
}

extern "C" void rush2_circuit_complete_v0(uint8_t* rdram, recomp_context* ctx) {
    complete_at_length(rdram, ctx->r2);
}

// func_800A952C at 0x800A9670: $t9 = the index of the race just run, about to be compared with 27 (the last race).
extern "C" void rush2_circuit_last_race(uint8_t* rdram, recomp_context* ctx) {
    if ((int32_t)ctx->r25 >= rush2::circuit::length(rdram) - 1) ctx->r25 = stock_length - 1;
}

// func_803BCFA4 at 0x803BCFE4: $s3 = 28, the races left being 28 - finished.
extern "C" void rush2_circuit_races_left(uint8_t* rdram, recomp_context* ctx) {
    int races = std::max(rush2::circuit::length(rdram), finished(rdram));
    ctx->r19 = (uint64_t)(int64_t)races;
}

namespace {
    using namespace recompui;

    bool expanded = false; // UI thread: whether the section is open.

    const char* const length_description =
        "Sets how many races a circuit has. Rush 2's circuits have 28. "
        "Applies to the next circuit you start; a circuit in progress keeps its own length.";
    const char* const courses_description =
        "Picks the courses circuits are made from. The toggles at the top turn all of a game's courses on or off. "
        "A course that isn't unlocked yet is skipped, and with none on, every course is used. "
        "Applies to the next circuit you start; a circuit in progress keeps its own courses.";

    bool game_shown(const Game& g) {
        if (g.first == rush2::track2049::first_menu_id) return rush2::track2049::available();
        if (g.first == rush2::track1::first_menu_id) return rush2::track1::available();
        return true;
    }

    bool all_on(const Game& g) {
        for (int t = g.first; t < g.first + g.count; t++) {
            if (!next_courses[t].load()) return false;
        }
        return true;
    }

    // A line of the section: a name on the left, a toggle on the right.
    Toggle* add_toggle_line(Element* parent, const std::string& name, bool bold, bool checked) {
        ContextId context = get_current_context();
        Element* line = context.create_element<rush2::ui::FocusRow>(parent);
        line->set_display(Display::Flex);
        line->set_flex_direction(FlexDirection::Row);
        line->set_align_items(AlignItems::Center);
        line->set_gap(16.0f);
        line->set_as_navigation_container(NavigationType::Horizontal);
        Label* label = context.create_element<Label>(line, name, bold ? theme::Typography::LabelSM : theme::Typography::Body);
        if (!bold) label->set_color(theme::color::TextDim);
        label->set_flex_grow(1.0f);
        Toggle* toggle = context.create_element<Toggle>(line, ToggleSize::Medium);
        toggle->set_flex_shrink(0.0f);
        toggle->set_checked(checked);
        return toggle;
    }

    // The section: a heading with a button that opens and closes the course list below it. At the top, a toggle per
    // game turns all its courses on or off; below, a toggle per course.
    class Section : public Element {
    public:
        Section(ResourceId rid, Element* parent, const rush2::circuit::TextShower& text_shower)
            : Element(rid, parent, Events(EventType::Update), "div", false) {
            ContextId context = get_current_context();
            set_display(Display::Flex);
            set_flex_direction(FlexDirection::Column);
            set_padding(12.0f);
            set_gap(8.0f);
            set_width(100.0f, Unit::Percent);
            set_as_navigation_container(NavigationType::Vertical);

            // Circuit Length: a select of 4 to 28 races.
            Element* length_row = context.create_element<rush2::ui::FocusRow>(this, text_shower(length_description));
            length_row->set_display(Display::Flex);
            length_row->set_flex_direction(FlexDirection::Row);
            length_row->set_align_items(AlignItems::Center);
            length_row->set_gap(16.0f);
            length_row->set_as_navigation_container(NavigationType::Horizontal);
            Label* length_label = context.create_element<Label>(length_row, "Circuit Length", theme::Typography::LabelMD);
            length_label->set_flex_grow(1.0f);
            length_label->set_flex_shrink(0.0f);
            std::vector<SelectOption> lengths;
            for (int races = 4; races <= stock_length; races += 4) {
                lengths.emplace_back(std::to_string(races) + (races == stock_length ? " Races (Rush 2)" : " Races"),
                                     std::to_string(races));
            }
            int chosen = next_length.load();
            if (chosen % 4 != 0) {
                lengths.emplace_back(std::to_string(chosen) + " Races", std::to_string(chosen));
            }
            Select* select = context.create_element<Select>(length_row, lengths, std::to_string(chosen));
            // Select is focusable but not in the tab order, which the d-pad's navigation (RmlUi's nav: auto) needs.
            select->set_tab_index_auto();
            select->add_change_callback([](SelectOption& option, int) {
                int races = std::clamp(std::stoi(option.value), 1, stock_length);
                next_length.store(races);
                circuit_config.update_option_value(length_id, (double)races);
                circuit_config.save_config();
            });

            Element* head = context.create_element<rush2::ui::FocusRow>(this, text_shower(courses_description));
            head->set_display(Display::Flex);
            head->set_flex_direction(FlexDirection::Row);
            head->set_align_items(AlignItems::Center);
            head->set_gap(16.0f);
            head->set_as_navigation_container(NavigationType::Horizontal);
            Label* title = context.create_element<Label>(head, "Circuit Courses", theme::Typography::LabelMD);
            title->set_flex_grow(1.0f);
            button = context.create_element<Button>(head, expanded ? "Hide" : "Show", ButtonStyle::Secondary);
            button->set_flex_shrink(0.0f);
            button->add_pressed_callback([this]() {
                expanded = !expanded;
                queue_update();
            });

            body = context.create_element<Element>(this, 0, "div", false);
            body->set_display(Display::Flex);
            body->set_flex_direction(FlexDirection::Column);
            body->set_gap(6.0f);
            body->set_as_navigation_container(NavigationType::Vertical);
            build();
        }

    protected:
        std::string_view get_type_name() override { return "CircuitCoursesSection"; }

        void process_event(const Event& e) override {
            if (e.type == EventType::Update && built != expanded) {
                button->set_text(expanded ? "Hide" : "Show");
                build();
            }
        }

    private:
        Button* button = nullptr;
        Element* body = nullptr;
        bool built = false;
        std::vector<std::pair<int, Toggle*>> game_toggles;  // (index in games, toggle)
        std::vector<std::pair<int, Toggle*>> course_toggles; // (track, toggle)

        void refresh() {
            for (auto [g, toggle] : game_toggles) toggle->set_checked(all_on(games[g]));
            for (auto [t, toggle] : course_toggles) toggle->set_checked(next_courses[t].load());
        }

        void build() {
            body->clear_children();
            game_toggles.clear();
            course_toggles.clear();
            built = expanded;
            if (!expanded) return;
            ContextId context = get_current_context();

            Element* all = context.create_element<Element>(body, 0, "div", false);
            all->set_display(Display::Flex);
            all->set_flex_direction(FlexDirection::Column);
            all->set_gap(6.0f);
            all->set_padding_bottom(8.0f);
            all->set_as_navigation_container(NavigationType::Vertical);
            for (int g = 0; g < (int)std::size(games); g++) {
                if (!game_shown(games[g])) continue;
                Toggle* toggle = add_toggle_line(all, std::string("All ") + games[g].name + " Courses", true,
                                                 all_on(games[g]));
                toggle->add_checked_callback([this, g](bool on) {
                    for (int t = games[g].first; t < games[g].first + games[g].count; t++) set_course(t, on);
                    circuit_config.save_config();
                    refresh();
                });
                game_toggles.emplace_back(g, toggle);
            }

            for (int g = 0; g < (int)std::size(games); g++) {
                if (!game_shown(games[g])) continue;
                Element* block = context.create_element<Element>(body, 0, "div", false);
                block->set_display(Display::Flex);
                block->set_flex_direction(FlexDirection::Column);
                block->set_gap(6.0f);
                block->set_padding_top(8.0f);
                block->set_as_navigation_container(NavigationType::Vertical);
                context.create_element<Label>(block, games[g].name, theme::Typography::LabelSM);
                for (int t = games[g].first; t < games[g].first + games[g].count; t++) {
                    Toggle* toggle = add_toggle_line(block, course_name(t), false, next_courses[t].load());
                    toggle->add_checked_callback([this, t](bool on) {
                        set_course(t, on);
                        circuit_config.save_config();
                        refresh();
                    });
                    course_toggles.emplace_back(t, toggle);
                }
            }
        }
    };
}

void rush2::circuit::add_section(recompui::Element* parent, const TextShower& text_shower) {
    recompui::get_current_context().create_element<Section>(parent, text_shower);
}
