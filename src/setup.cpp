// Setup (include/setup.h, docs/settings_menu.md#setup): the game rules presets and the screen that picks one.
//
// The profile is kept in players.json's "setup" section (include/data_files.h):
//   { "profile": "original" | "enhanced", "pending": true, "known": [ "general/torque_rebalance", ... ] }
// "known" lists the rule settings the profile has been applied to. A rule setting added in a later version isn't in
// it, so the next launch gives it the profile's value: an Original player's new setting starts at Rush 2's behavior,
// an Enhanced player's at its default. "pending" is set while a fresh install hasn't finished the setup screen.

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
    enum class Cfg { General, Games, Cheats };

    // A setting that changes how Rush 2 itself plays, with Rush 2's value. Enhanced is the setting's default. Settings
    // that only change the content of the other games, the controls, the cameras or the graphics aren't here.
    struct Rule {
        Cfg cfg;
        const char* id;
        ConfigValueVariant original;
    };

    const std::vector<Rule>& rules() {
        static const std::vector<Rule> list = {
            { Cfg::General, "accurate_car_stats", false },
            { Cfg::General, "torque_rebalance", false },
            { Cfg::General, "stunt_first_gear", false },
            { Cfg::General, "hide_respawning_cars", false },
            { Cfg::General, "drone_catch_up", true },
            { Cfg::General, "pause_menu_extras", false },
            { Cfg::General, "accurate_circuit_review", false },
            { Cfg::General, "more_circuit_settings", false },
            { Cfg::General, "circuit_fixed_difficulty", false },
            { Cfg::Games, "wings", false },
            { Cfg::Games, "car_speeds", static_cast<uint32_t>(rush2::car2049::SpeedMode::Rush2) },
            { Cfg::Games, "stunt_scoring", static_cast<uint32_t>(rush2::stunt2049::Scoring::Rush2) },
            { Cfg::Games, "rush2049_odometer", false },
            { Cfg::Cheats, "unlock_system", false },
        };
        return list;
    }

    recomp::config::Config& config_of(Cfg cfg) {
        switch (cfg) {
            case Cfg::Games: return rush2::wings::games_config();
            case Cfg::Cheats: return recompui::config::get_config("cheats");
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
        static const char* const names[] = { "general", "games", "cheats" };
        return std::string(names[(int)r.cfg]) + "/" + r.id;
    }

    const recomp::config::ConfigOption* option_of(const Rule& r) {
        const auto& schema = config_of(r.cfg).get_config_schema();
        auto it = schema.options_by_id.find(r.id);
        return it == schema.options_by_id.end() ? nullptr : &schema.options[it->second];
    }

    ConfigValueVariant default_of(const recomp::config::ConfigOption& option) {
        return std::visit([](const auto& v) -> ConfigValueVariant { return v.default_value; }, option.variant);
    }

    ConfigValueVariant value_for(const Rule& r, Profile p) {
        return p == Profile::Original ? r.original : default_of(*option_of(r));
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
            if (option_of(r) != nullptr && config_of(r.cfg).get_option_value(r.id) != value_for(r, p)) {
                return false;
            }
        }
        return true;
    }

    // Sets every rule setting to the preset's value, each through its config so it takes effect as its description
    // says, and saves them.
    void apply(Profile p) {
        bool changed[3] = {};
        for (const Rule& r : rules()) {
            if (option_of(r) == nullptr) continue;
            ConfigValueVariant v = value_for(r, p);
            if (config_of(r.cfg).get_option_value(r.id) != v) {
                config_of(r.cfg).update_option_value(r.id, v);
                changed[(int)r.cfg] = true;
            }
        }
        for (int c = 0; c < 3; c++) {
            if (changed[c]) save((Cfg)c);
        }
        profile = p;
        write_state();
    }

    // The names of the settings a preset changes from the other's value, for the setup screen's descriptions.
    std::string differing_names() {
        std::string out;
        for (const Rule& r : rules()) {
            const recomp::config::ConfigOption* option = option_of(r);
            if (option == nullptr || r.original == default_of(*option)) continue;
            out += (out.empty() ? "" : ", ") + option->name;
        }
        return out;
    }

    // The setup screen: step 0 picks the game rules, step 1 is the Games tab's page (the other games' ROMs).
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
            done_button = context.create_element<Button>(right, "Done", ButtonStyle::Primary, ButtonSize::Medium);
            done_button->add_pressed_callback([this]() { close(); });
            set_menu_action_callback(MenuAction::Back, [this]() { go_back(); }, "Back");
            queue_update(); // Modal::process_event queues the next one each frame.
        }

        void start(bool first) {
            first_launch = first;
            wanted_step = 0;
            shown_step = -1;
        }

    protected:
        std::string_view get_type_name() override { return "Rush2SetupModal"; }

        void process_event(const recompui::Event& e) override {
            // Built here rather than in a button's callback, which would delete the button it runs in.
            if (e.type == recompui::EventType::Update && is_open && wanted_step != shown_step) {
                shown_step = wanted_step;
                build_step();
            }
            Modal::process_event(e);
        }

    private:
        recompui::Label* title = nullptr;
        recompui::Button* back_button = nullptr;
        recompui::Button* done_button = nullptr;
        bool first_launch = false;
        int wanted_step = 0;
        int shown_step = -1;

        void go_back() {
            if (wanted_step == 1) {
                rush2::wings::save_config();
                wanted_step = 0;
            }
            else {
                close();
            }
        }

        void choose(Profile p) {
            if (first_launch || rush2::setup::rules_name() != "Custom") {
                apply(p);
                wanted_step = 1;
                return;
            }
            std::string name = p == Profile::Original ? "Original" : "Enhanced";
            recompui::open_choice_prompt(
                "Use " + name + " game rules?",
                "Your own changes to the game rule settings are replaced. Controls, graphics, saves, unlocks and the "
                "other games' tracks and cars are kept.",
                "Use " + name,
                "Cancel",
                [this, p]() {
                    apply(p);
                    wanted_step = 1;
                },
                []() {},
                recompui::ButtonStyle::Success,
                recompui::ButtonStyle::Danger,
                true);
        }

        void build_step() {
            using namespace recompui;
            ContextId context = modal_root_context;
            body->clear_children();
            back_button->set_display(shown_step == 1 ? Display::Flex : Display::None);
            done_button->set_text(shown_step == 1 ? "Done" : "Skip");
            if (shown_step == 1) {
                title->set_text("Setup: Other Games (optional)");
                rush2::games::create_page(context, body);
                done_button->focus();
                return;
            }
            title->set_text("Setup: Game Rules");
            const std::string names = differing_names();
            auto* page = context.create_element<rush2::ui::OptionsPage>(body,
                "Pick how Rush 2 plays. Either choice keeps the tracks and cars added from other games, and the "
                "controls and graphics settings.<br/><br/>Every setting can still be changed one by one in the General, "
                "Games and Progress tabs, and <recomp-color primary>Run Setup Again</recomp-color> on the General tab "
                "comes back here.");
            page->add_heading("Preset", first_launch ? "" : "Now: " + rush2::setup::rules_name());
            auto add_choice = [&](const std::string& label, const std::string& description, std::function<void()> pressed) {
                Element* row = page->add_row(description);
                auto* button = context.create_element<Button>(row, label, ButtonStyle::Secondary);
                button->add_pressed_callback(std::move(pressed));
                return button;
            };
            Button* original = add_choice("Original",
                "<recomp-color primary>Original</recomp-color> plays by Rush 2's own rules, as on the N64. Turns off: " +
                names + ".", [this]() { choose(Profile::Original); });
            Button* enhanced = add_choice("Enhanced",
                "<recomp-color primary>Enhanced</recomp-color> adds the recompiled version's changes to how Rush 2 "
                "plays: " + names + ".", [this]() { choose(Profile::Enhanced); });
            Button* keep = nullptr;
            if (!first_launch) {
                keep = add_choice("Keep Current", "Leaves the game rule settings as they are (" +
                    rush2::setup::rules_name() + ") and goes on to the other games.", [this]() { wanted_step = 1; });
            }
            Button* focus = profile == Profile::Original ? original : enhanced;
            if (keep != nullptr && rush2::setup::rules_name() == "Custom") {
                focus = keep;
            }
            focus->focus();
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

    // Rule settings new since the profile was last applied take the profile's value.
    bool changed[3] = {};
    for (const Rule& r : rules()) {
        if (option_of(r) == nullptr) {
            fprintf(stderr, "[setup] rule setting %s isn't registered\n", key_of(r).c_str());
            continue;
        }
        std::string key = key_of(r);
        if (std::find(known.begin(), known.end(), key) != known.end()) continue;
        known.push_back(key);
        ConfigValueVariant v = value_for(r, profile);
        if (j.is_object() && config_of(r.cfg).get_option_value(r.id) != v) {
            config_of(r.cfg).update_option_value(r.id, v);
            changed[(int)r.cfg] = true;
        }
    }
    for (int c = 0; c < 3; c++) {
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
            rush2::wings::save_config();
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
