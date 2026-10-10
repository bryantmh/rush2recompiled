// Games tab: the San Francisco Rush 2049 and San Francisco Rush ROMs, each with what it adds to the game (src/rush2049/wings.cpp,
// src/rush1/rush1_rom.cpp), and the settings of the added battles. All of it is one config, games.json
// (rush2::wings::games_config()).

// As in src/rush2049/wings.cpp: librecomp's nlohmann::json first.
#include "../lib/N64ModernRuntime/thirdparty/json/json.hpp"

#include <functional>
#include <string>
#include <vector>

#include "recompui/config.h"
#include "librecomp/config.hpp"

#include "rush2.h"
#include "battle.h"
#include "options_page.h"
#include "track1.h"
#include "track2049.h"
#include "wings.h"
#include "wings_internal.h"

namespace {
    using namespace recompui;

    const std::string tab_id = "games";
    const std::string tab_name = "Games";

    // Hidden: the old battle time limit, read once into the BATTLE select's TIME LIMIT (src/rush2049/track2049_menu.cpp,
    // rush2::battle::legacy_time_limit_minutes).
    const std::string battle_time_option_id = "battle_time_limit";
    const std::string fire_backward_option_id = "battle_fire_backward";

    void add_battle_options() {
        recomp::config::Config& config = rush2::wings::games_config();
        config.add_enum_option(
            battle_time_option_id,
            "Battle Time Limit",
            "How long a Rush 2049 battle arena lasts (Start Game > Battle). Takes effect at the next race. Requires a "
            "Rush 2049 (USA) ROM.",
            {
                { rush2::battle::TimeLimit::One, "One", "1 Minute" },
                { rush2::battle::TimeLimit::Two, "Two", "2 Minutes" },
                { rush2::battle::TimeLimit::Three, "Three", "3 Minutes" },
                { rush2::battle::TimeLimit::Five, "Five", "5 Minutes" },
                { rush2::battle::TimeLimit::Ten, "Ten", "10 Minutes" },
            },
            rush2::battle::TimeLimit::Three,
            true
        );
        config.add_option_change_callback(battle_time_option_id,
            [](recomp::config::ConfigValueVariant cur_value, recomp::config::ConfigValueVariant, recomp::config::OptionChangeContext) {
                rush2::battle::set_time_limit(static_cast<rush2::battle::TimeLimit>(std::get<uint32_t>(cur_value)));
            });
        // Not in Rush 2049 (its weapons fire ahead only).
        config.add_bool_option(
            fire_backward_option_id,
            "Fire Backward",
            "In a battle (the battle arenas, a race with the track select's BATTLE row on, or the Weapons cheat), "
            "holding the steering stick back while firing shoots the cannon, gatling, rockets, missile and grenades "
            "behind the car instead of ahead (on a keyboard, the down arrow). Takes effect at once. Requires a Rush "
            "2049 (USA) ROM.",
            true
        );
        config.add_option_change_callback(fire_backward_option_id,
            [](recomp::config::ConfigValueVariant cur_value, recomp::config::ConfigValueVariant, recomp::config::OptionChangeContext) {
                rush2::battle::set_fire_backward(std::get<bool>(cur_value));
            });
    }

    // The battles come with the Rush 2049 tracks.
    void update_battle_options(bool battles) {
        recomp::config::Config& config = rush2::wings::games_config();
        config.update_option_disabled(battle_time_option_id, !battles);
        config.update_option_disabled(fire_backward_option_id, !battles);
    }

    const std::string description =
        "Adds the tracks, cars and wings of San Francisco Rush 2049 and the tracks of San Francisco Rush, from their "
        "USA N64 ROMs.\n\n"
        "Select each game's ROM or disc once; it is copied into the app folder.";

    class GamesPage : public rush2::ui::OptionsPage {
    public:
        GamesPage(ResourceId rid, Element* parent) : OptionsPage(rid, parent, html(description)) {
            rush2::wings::add_games_section(this, refresh[0]);
            add_option(rush2::wings::games_config(), fire_backward_option_id);
            rush2::track1::add_games_section(this, refresh[1]);
            update_battle_options(battles);
        }

    protected:
        std::string_view get_type_name() override { return "GamesPage"; }

        void on_update() override {
            for (auto& f : refresh) {
                if (f) {
                    f();
                }
            }
            if (rush2::track2049::available() != battles) {
                battles = !battles;
                update_battle_options(battles);
            }
        }

    private:
        std::function<void()> refresh[2];
        bool battles = rush2::track2049::available();

        static std::string html(const std::string& text) {
            std::string out;
            for (char c : text) {
                out += c == '\n' ? std::string("<br/>") : std::string(1, c);
            }
            return out;
        }
    };
}

void rush2::games::create_tab() {
    rush2::wings::init_config();
    rush2::track1::init_config();
    add_battle_options();
    recompui::config::create_tab(tab_name, tab_id,
        [](ContextId context, Element* parent) {
            context.create_element<GamesPage>(parent);
        },
        nullptr,
        [](TabCloseContext) {
            // Both games' options are one config.
            rush2::wings::save_config();
        });
}
