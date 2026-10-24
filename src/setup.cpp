// Setup (include/setup.h, docs/settings_menu.md#setup): the game rules presets and the screen that picks one.
//
// The profile is kept in players.json's "setup" section (include/data_files.h):
//   { "profile": "original" | "enhanced", "pending": true, "known": [ "general/torque_rebalance", ... ] }
// "known" lists the rule settings the profile has been applied to. A rule setting added in a later version isn't in
// it, so the next launch gives it the profile's value: an Original player's new setting starts at Rush 2's behavior,
// an Enhanced player's at Enhanced's. "pending" is set while a fresh install hasn't finished the setup screen.

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <string>
#include <vector>

#include "json/json.hpp"

#include "recompui/recompui.h"
#include "recompui/config.h"
#include "librecomp/config.hpp"
#include "elements/ui_button.h"
#include "elements/ui_label.h"
#include "elements/ui_modal.h"

#include "car2049.h"
#include "data_files.h"
#include "options_page.h"
#include "rush2.h"
#include "setup.h"
#include "sim_run.h"
#include "stunt2049.h"
#include "wings.h"
#include "wings_internal.h"

namespace {
    using rush2::setup::Profile;
    using recomp::config::ConfigValueVariant;

    const std::string section = "setup";

    // The configs rule settings live in.
    enum class Cfg { General, Games, Cheats, Graphics };
    constexpr int cfg_count = 4;

    // A setting a preset sets: one that changes how Rush 2 itself plays, or which other games' cars race on the other
    // games' tracks. Settings that only change the other games' content, the controls, the cameras or the graphics
    // aren't here. group is its column on the Game Rules step (groups below), or -1 for one on the Other Games step
    // (the Rush 2049 and California Speed settings, which mean nothing before those games are added). keep and change
    // are what Original and Enhanced do, as their cards list it.
    struct Rule {
        Cfg cfg;
        const char* id;
        ConfigValueVariant original;
        ConfigValueVariant enhanced;
        int group;
        const char* keep = nullptr;
        const char* change = nullptr;
    };

    const char* const groups[] = { "Cars and Races", "Menus and Circuits", "Graphics" };

    const std::vector<Rule>& rules() {
        using rush2::car2049::DroneCars;
        constexpr uint32_t own_tracks = static_cast<uint32_t>(DroneCars::Rush2049Tracks);
        constexpr uint32_t all_tracks = static_cast<uint32_t>(DroneCars::AllTracks);
        constexpr uint32_t rush2_speeds = static_cast<uint32_t>(rush2::car2049::SpeedMode::Rush2);
        static const std::vector<Rule> list = {
            { Cfg::General, "accurate_car_stats", false, true, 0, "Car select stat bars use Rush 2's formulas",
              "Car select stat bars measured from the real physics" },
            { Cfg::General, "torque_rebalance", false, true, 0, "Rush 2's TORQUE curves", "Each TORQUE setting has a real use" },
            { Cfg::General, "stunt_first_gear", false, true, 0, "Respawns start in 2nd gear (manual gearbox)",
              "Respawns start in 1st gear (manual gearbox)" },
            { Cfg::General, "hide_respawning_cars", false, true, 0 },
            { Cfg::General, "drone_catch_up", true, true, 0 },
            { Cfg::General, "pause_menu_extras", false, true, 1, "Rush 2's pause menu",
              "Pause menu adds SKIP TRACK, song and collectibles" },
            { Cfg::General, "accurate_circuit_review", false, true, 1, "Rush 2's tour of its tracks after a circuit",
              "The end of a circuit shows the tracks you raced" },
            { Cfg::General, "more_circuit_settings", false, true, 1, "DEATHS only set before a circuit's first race",
              "Set DIFFICULTY and DEATHS in any circuit race" },
            { Cfg::General, "circuit_fixed_difficulty", false, false, 1 },
            { Cfg::Cheats, "unlock_system", false, true, 1, "Cars and tracks unlock as in Rush 2",
              "Collectibles earn points to buy cars, tracks and parts" },
            { Cfg::Games, "wings", false, true, -1 },
            { Cfg::Games, "stunt_scoring", static_cast<uint32_t>(rush2::stunt2049::Scoring::Rush2),
              static_cast<uint32_t>(rush2::stunt2049::Scoring::Rush2049), -1 },
            { Cfg::Games, "rush2049_odometer", false, false, -1 },
            { Cfg::Games, "car_speeds", rush2_speeds, rush2_speeds, -1 },
            { Cfg::Games, "rush2049_computer_cars", own_tracks, all_tracks, -1 },
            { Cfg::Games, "calspeed_computer_cars", own_tracks, all_tracks, -1 },
        };
        return list;
    }

    // Shown on the Game Rules step to turn on with either preset, but no preset changes them (they stay off).
    struct Extra {
        Cfg cfg;
        const char* id;
        int group;
    };
    const Extra extras[] = {
        { Cfg::Graphics, "ambient_occlusion", 2 },
        { Cfg::Graphics, "car_sheen", 2 },
    };

    recomp::config::Config& config_of(Cfg cfg) {
        switch (cfg) {
            case Cfg::Games: return rush2::wings::games_config();
            case Cfg::Cheats: return recompui::config::get_config("cheats");
            case Cfg::Graphics: return recompui::config::get_graphics_config();
            default: return recompui::config::get_general_config();
        }
    }

    void save(Cfg cfg) {
        if (cfg == Cfg::Games) {
            rush2::wings::save_config();
        }
        else {
            config_of(cfg).save_config();
        }
    }

    std::string key_of(const Rule& r) {
        static const char* const names[] = { "general", "games", "cheats", "graphics" };
        return std::string(names[(int)r.cfg]) + "/" + r.id;
    }

    bool registered(Cfg cfg, const char* id) {
        return config_of(cfg).get_config_schema().options_by_id.contains(id);
    }

    ConfigValueVariant value_for(const Rule& r, Profile p) {
        return p == Profile::Original ? r.original : r.enhanced;
    }

    // Saves every config the setup screen's options are in (the menu's tabs save theirs when they close).
    void save_all() {
        for (int c = 0; c < cfg_count; c++) {
            save((Cfg)c);
        }
    }

    Profile profile = Profile::Enhanced;
    bool is_pending = false;
    std::vector<std::string> known;

    void write_state() {
        nlohmann::json j{ { "profile", profile == Profile::Original ? "original" : "enhanced" }, { "known", known } };
        if (is_pending) {
            j["pending"] = true;
        }
        rush2::data_files::write(rush2::data_files::File::Players, section, j.dump());
    }

    bool matches(Profile p) {
        for (const Rule& r : rules()) {
            if (registered(r.cfg, r.id) && config_of(r.cfg).get_option_value(r.id) != value_for(r, p)) {
                return false;
            }
        }
        return true;
    }

    // Sets every rule setting to the preset's value, each through its config so it takes effect as its description
    // says, and saves them.
    void apply(Profile p) {
        bool changed[cfg_count] = {};
        for (const Rule& r : rules()) {
            if (!registered(r.cfg, r.id)) continue;
            ConfigValueVariant v = value_for(r, p);
            if (config_of(r.cfg).get_option_value(r.id) != v) {
                config_of(r.cfg).update_option_value(r.id, v);
                changed[(int)r.cfg] = true;
            }
        }
        for (int c = 0; c < cfg_count; c++) {
            if (changed[c]) save((Cfg)c);
        }
        profile = p;
        write_state();
    }

    // The setup screen: step 0 picks the game rules (two cards, then their settings to customize), step 1 is the Games
    // tab's page (the other games' ROMs).
    class SetupModal : public recompui::Modal {
    public:
        SetupModal(recompui::ResourceId rid, recompui::Document* parent, recompui::ContextId context)
            : Modal(rid, parent, context, recompui::ModalType::Fullscreen) {
            using namespace recompui;
            title = context.create_element<Label>(header->get_left(), "", LabelStyle::Large);
            title->set_margin_left(16.0f);
            title->set_padding_top(20.0f);
            title->set_padding_bottom(20.0f);
            Element* right = header->get_right();
            right->set_gap(8.0f);
            back_button = context.create_element<Button>(right, "Back", ButtonStyle::Secondary, ButtonSize::Medium);
            back_button->add_pressed_callback([this]() { go_back(); });
            next_button = context.create_element<Button>(right, "Next", ButtonStyle::Primary, ButtonSize::Medium);
            next_button->add_pressed_callback([this]() {
                if (shown_step == 0) {
                    save_all();
                    wanted_step = 1;
                }
                else {
                    close();
                }
            });
            set_menu_action_callback(MenuAction::Back, [this]() { go_back(); }, "Back");
            set_menu_action_callback(MenuAction::Toggle, [this]() { toggle(); }, "Close");
            queue_update(); // Modal::process_event queues the next one each frame.
        }

        void start(bool first) {
            first_launch = first;
            picked = !first;
            wanted_step = 0;
            shown_step = -1;
        }

    protected:
        std::string_view get_type_name() override { return "Rush2SetupModal"; }

        void process_event(const recompui::Event& e) override {
            if (e.type == recompui::EventType::Update && is_open) {
                // Built here rather than in a button's callback, which would delete the button it runs in.
                if (wanted_step != shown_step) {
                    shown_step = wanted_step;
                    build_step();
                }
                if (shown_step == 0) {
                    update_cards();
                }
            }
            Modal::process_event(e);
        }

    private:
        recompui::Label* title = nullptr;
        recompui::Button* back_button = nullptr;
        recompui::Button* next_button = nullptr;
        recompui::Button* cards[2] = {};
        recompui::Element* customize = nullptr;
        recompui::Label* customize_note = nullptr;
        std::string shown_rules;
        bool customize_shown = false;
        bool first_launch = false;
        bool picked = false;    // the settings to customize show once a card is picked (at once from the menu)
        int wanted_step = 0;
        int shown_step = -1;

        // Escape or Start: on a first launch's Game Rules step it goes on to the other games (where the ROMs are added)
        // rather than closing the screen past them.
        bool toggle_is_next() const {
            return first_launch && shown_step == 0;
        }

        void toggle() {
            if (toggle_is_next()) {
                save_all();
                wanted_step = 1;
            }
            else {
                close();
            }
        }

        void go_back() {
            if (wanted_step == 1) {
                save_all();
                wanted_step = 0;
            }
            else {
                close();
            }
        }

        void pick(Profile p) {
            std::string name = p == Profile::Original ? "Original" : "Enhanced";
            if (first_launch || rush2::setup::rules_name() != "Custom") {
                apply(p);
                picked = true;
                return;
            }
            recompui::open_choice_prompt(
                "Use " + name + "?",
                "This replaces your changes to the settings below. Controls, graphics, saves and unlocks are kept.",
                "Use " + name,
                "Cancel",
                [this, p]() {
                    apply(p);
                    picked = true;
                },
                []() {},
                recompui::ButtonStyle::Success,
                recompui::ButtonStyle::Danger,
                true);
        }

        // A preset's card, all one button: its name, one line on what it is and what it does for each setting.
        recompui::Button* add_card(recompui::Element* parent, Profile p) {
            using namespace recompui;
            ContextId context = modal_root_context;
            bool original = p == Profile::Original;
            auto* card = context.create_element<Button>(parent, "", ButtonStyle::Secondary);
            // Laid out as a card rather than as the button's one centered line.
            card->set_flex_direction(FlexDirection::Column);
            card->set_align_items(AlignItems::FlexStart);
            card->set_justify_content(JustifyContent::FlexStart);
            card->set_width(560.0f);
            card->set_height(340.0f);
            card->set_min_height(340.0f);
            card->set_max_height(340.0f);
            card->set_padding(20.0f);
            card->set_padding_left(24.0f);
            card->set_padding_right(24.0f);
            card->set_border_radius(theme::border::radius_lg);
            context.create_element<Label>(card, original ? "Original" : "Enhanced", theme::Typography::Header2);
            auto* summary = context.create_element<Label>(card,
                original ? "Rush 2 exactly as it played on the N64:" : "Rush 2 with these changes:", theme::Typography::Body);
            summary->set_margin_top(8.0f);
            summary->set_color(theme::color::Text);
            Element* list = context.create_element<Element>(card, 0, "div", false);
            list->set_margin_top(8.0f);
            for (const Rule& r : rules()) {
                const char* text = original ? r.keep : r.change;
                if (text == nullptr) continue;
                auto* line = context.create_element<Label>(list, std::string("\xE2\x80\xA2 ") + text, theme::Typography::Body);
                line->set_color(theme::color::TextDim);
                line->set_display(Display::Block);
            }
            card->add_pressed_callback([this, p]() { pick(p); });
            return card;
        }

        // Marks the card matching the settings and shows the settings once one is picked.
        void update_cards() {
            using namespace recompui;
            std::string now = rush2::setup::rules_name();
            if (now == shown_rules && customize_shown == picked) return;
            shown_rules = now;
            customize_shown = picked;
            for (int i = 0; i < 2; i++) {
                bool on = now == (i == 0 ? "Original" : "Enhanced");
                cards[i]->apply_button_style(on ? ButtonStyle::Primary : ButtonStyle::Secondary);
            }
            customize->set_display(picked ? Display::Block : Display::None);
            customize_note->set_text(now == "Custom" ? "Customized" : "As " + now);
        }

        void build_step() {
            using namespace recompui;
            ContextId context = modal_root_context;
            body->clear_children();
            back_button->set_display(shown_step == 1 ? Display::Flex : Display::None);
            next_button->set_text(shown_step == 1 ? "Done" : "Next");
            set_menu_action_callback(MenuAction::Toggle, [this]() { toggle(); }, toggle_is_next() ? "Next" : "Close");
            if (shown_step == 1) {
                title->set_text("Setup: Other Games (optional)");
                rush2::games::create_page(context, body);
                next_button->focus();
                return;
            }
            title->set_text("Setup: Game Rules");
            shown_rules.clear();
            customize_shown = false;

            Element* page = context.create_element<Element>(body, 0, "div", false);
            page->set_display(Display::Block);
            page->set_position(Position::Relative);
            page->set_width(100.0f, Unit::Percent);
            page->set_height(100.0f, Unit::Percent);
            page->set_as_navigation_container(NavigationType::Vertical);

            Element* row = context.create_element<Element>(page, 0, "div", false);
            row->set_position(Position::Absolute);
            row->set_top(24.0f);
            row->set_left(0.0f);
            row->set_right(0.0f);
            row->set_display(Display::Flex);
            row->set_flex_direction(FlexDirection::Row);
            row->set_justify_content(JustifyContent::Center);
            row->set_gap(32.0f);
            row->set_as_navigation_container(NavigationType::Horizontal);
            cards[0] = add_card(row, Profile::Original);
            cards[1] = add_card(row, Profile::Enhanced);
            auto* kept = context.create_element<Label>(page,
                "Both keep your controls and graphics settings. The other games' settings follow on the next page.",
                theme::Typography::Body);
            kept->set_position(Position::Absolute);
            kept->set_top(380.0f);
            kept->set_left(0.0f);
            kept->set_right(0.0f);
            kept->set_text_align(TextAlign::Center);
            kept->set_color(theme::color::TextDim);

            // The preset's settings, to turn on or off one by one.
            customize = context.create_element<Element>(page, 0, "div", false);
            customize->set_position(Position::Absolute);
            customize->set_top(424.0f);
            customize->set_left(0.0f);
            customize->set_right(0.0f);
            customize->set_bottom(0.0f);
            customize->set_border_top_width(1.0f);
            customize->set_border_top_color(theme::color::Border);
            customize->set_display(Display::None);
            auto* options = context.create_element<rush2::ui::OptionsPage>(customize,
                "Turn any of these on or off to customize the choice. Graphics stay off unless you turn them on. All of "
                "them are also in the General, Graphics and Progress tabs.", 3);
            for (int g = 0; g < 3; g++) {
                if (g > 0) options->next_column();
                rush2::ui::OptionsPage::Heading heading = options->add_heading(groups[g]);
                if (g == 0) customize_note = heading.note;
                for (const Rule& r : rules()) {
                    if (r.group == g && registered(r.cfg, r.id)) {
                        options->add_option(config_of(r.cfg), r.id);
                    }
                }
                for (const Extra& x : extras) {
                    if (x.group == g && registered(x.cfg, x.id)) {
                        options->add_option(config_of(x.cfg), x.id);
                    }
                }
            }
            update_cards();
            cards[profile == Profile::Original ? 0 : 1]->focus();
        }
    };

    SetupModal* modal = nullptr;
    bool opened_from_menu = false;
}

void rush2::setup::load(bool fresh_install) {
    std::string text = rush2::data_files::read(rush2::data_files::File::Players, section);
    nlohmann::json j = text.empty() ? nlohmann::json() : nlohmann::json::parse(text, nullptr, false);
    known.clear();
    if (j.is_object()) {
        profile = j.contains("profile") && j["profile"] == "original" ? Profile::Original : Profile::Enhanced;
        is_pending = j.contains("pending") && j["pending"] == true;
        if (j.contains("known") && j["known"].is_array()) {
            for (const auto& k : j["known"]) {
                if (k.is_string()) known.push_back(k.get<std::string>());
            }
        }
    }
    else {
        // No profile yet. An existing install keeps its settings; a fresh one shows the setup screen before the game.
        // Simulation runs start from defaults and never wait on a screen (unless testing the first launch itself).
        profile = !fresh_install && matches(Profile::Original) ? Profile::Original : Profile::Enhanced;
        is_pending = fresh_install && (!rush2::sim::active() || getenv("RUSH2_TEST_FIRST_BOOT") != nullptr);
    }

    // Rule settings new since the profile was last applied take the profile's value. A fresh install starts on
    // Enhanced's values, which the setup screen then changes.
    bool changed[cfg_count] = {};
    for (const Rule& r : rules()) {
        if (!registered(r.cfg, r.id)) {
            fprintf(stderr, "[setup] rule setting %s isn't registered\n", key_of(r).c_str());
            continue;
        }
        std::string key = key_of(r);
        if (std::find(known.begin(), known.end(), key) != known.end()) continue;
        known.push_back(key);
        ConfigValueVariant v = value_for(r, profile);
        if ((j.is_object() || is_pending) && config_of(r.cfg).get_option_value(r.id) != v) {
            config_of(r.cfg).update_option_value(r.id, v);
            changed[(int)r.cfg] = true;
        }
    }
    for (int c = 0; c < cfg_count; c++) {
        if (changed[c]) save((Cfg)c);
    }
    write_state();
}

bool rush2::setup::pending() {
    return is_pending;
}

std::string rush2::setup::rules_name() {
    return matches(Profile::Original) ? "Original" : matches(Profile::Enhanced) ? "Enhanced" : "Custom";
}

void rush2::setup::open(bool first_launch) {
    using namespace recompui;
    if (modal == nullptr) {
        // From a UI task, with no context open.
        ContextId context = create_context();
        context.open();
        modal = context.create_element<SetupModal>(context.get_root_element(), context);
        context.close();
        modal->set_on_close_callback([]() {
            save_all();
            if (is_pending) {
                // The launcher, shown again now that no menu is, goes on to the game.
                is_pending = false;
                write_state();
            }
            if (opened_from_menu) {
                // Back to the menu it was opened from, outside this context's event.
                recompui::queue_ui_task([]() { recompui::config::open(); });
            }
        });
    }
    hide_all_contexts();
    opened_from_menu = !first_launch;
    modal->start(first_launch);
    modal->open();
}
