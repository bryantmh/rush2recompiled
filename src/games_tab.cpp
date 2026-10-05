// Games tab: the San Francisco Rush 2049 and San Francisco Rush ROMs, each with what it adds to the game (src/wings.cpp,
// src/rush1_rom.cpp).

#include <functional>
#include <vector>

#include "recompui/config.h"

#include "rush2.h"
#include "options_page.h"
#include "track1.h"
#include "wings.h"

namespace {
    using namespace recompui;

    const std::string tab_id = "games";
    const std::string tab_name = "Games";

    const std::string description =
        "Adds the tracks, cars and wings of San Francisco Rush 2049 and the tracks of San Francisco Rush, from their "
        "USA N64 ROMs.\n\n"
        "Select each game's ROM once; it is copied into the app folder.";

    class GamesPage : public rush2::ui::OptionsPage {
    public:
        GamesPage(ResourceId rid, Element* parent) : OptionsPage(rid, parent, html(description)) {
            rush2::wings::add_games_section(this, refresh[0]);
            rush2::track1::add_games_section(this, refresh[1]);
        }

    protected:
        std::string_view get_type_name() override { return "GamesPage"; }

        void on_update() override {
            for (auto& f : refresh) {
                if (f) {
                    f();
                }
            }
        }

    private:
        std::function<void()> refresh[2];

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
    recompui::config::create_tab(tab_name, tab_id,
        [](ContextId context, Element* parent) {
            context.create_element<GamesPage>(parent);
        },
        nullptr,
        [](TabCloseContext) {
            rush2::wings::save_config();
            rush2::track1::save_config();
        });
}
