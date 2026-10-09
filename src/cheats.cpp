// Cheats tab.
//
// The game has a hidden Cheats screen (func_803B7710) with 27 items. It appears in the Setup menu once the byte at
// 0x800C213C is set (normally by holding Z+L+R and all four C buttons on the Setup screen), and each item can only be
// changed after its own unlock byte at 0x800C20E8 + i is set by entering that item's button code. Each item's state
// is a byte the game reads during races, and the menu shows the halfword at 0x800C2104 + 2 * i as the index of the
// value's label. Only the boot defaults function (func_800A00DC) and the cheat menu write these.
//
// The settings here are applied from the game's main loop (hook in us.toml):
// - Enable Cheat Menu sets the Setup menu flag and every item's unlock byte every frame, so all cheats can be
//   changed in game.
// - Each cheat's value is written once when it changes here, and once when the config loads (after the boot defaults
//   have run).
// - Changes made in the in-game cheat menu are copied back into this tab and saved. The config isn't thread safe, so
//   that only happens while no menu is open; the UI only uses the config while a menu is open, and game input (and
//   so the in-game cheat menu) is disabled then anyway.
//
// The tab also has three unlocks, which change nothing that is saved:
// - Unlock All Tracks forces the PIPE (0x800E7D50) and MIDWAY (0x800E7D19) availability bytes on every frame. The
//   game recomputes both from the save data in func_80094F1C, which is called again when the option is turned off.
//   It also opens the added tracks the unlock system locks (src/unlocks.cpp).
// - Unlock All Cars replaces each player's car list once car select (func_803B81F0) has built it. The game lists
//   cars 0-15, cars 16-19 for every 3 keys found on the current track, car 20 once 0x800C20D8 (or the profile's
//   +0x4B4) is set, and car 21 once all 4 cans on the current track are found; the Rush 2049 cars follow.
// - Unlock All Parts offers every ENGINE level on the Rush 2049 cars.
// Otherwise the unlock system decides what each player's car list holds (src/unlocks.cpp). Its own option, Unlock
// System, is kept in this tab's config but shown on the Progress tab.

#include <algorithm>
#include <array>
#include <atomic>
#include <functional>
#include <string>
#include <variant>
#include <vector>

#include "recompui/recompui.h"
#include "recompui/config.h"
#include "librecomp/config.hpp"
#include "elements/ui_button.h"
#include "elements/ui_config_page.h"
#include "elements/ui_label.h"
#include "elements/ui_select.h"
#include "elements/ui_toggle.h"

#include "recomp.h"
#include "rush2_hooks.h"
#include "rush2.h"
#include "battle.h"
#include "car2049.h"
#include "track2049.h"
#include "unlocks.h"

extern "C" void unlocks_check_tracks_80094F1C(uint8_t* rdram, recomp_context* ctx);

namespace {
    constexpr uint32_t cheat_menu_flag = 0x800C213C;
    constexpr uint32_t cheat_unlocked = 0x800C20E8;
    constexpr uint32_t cheat_display = 0x800C2104;
    constexpr int num_menu_items = 27;

    constexpr uint32_t pipe_unlocked = 0x800E7D50;
    constexpr uint32_t midway_unlocked = 0x800E7D19;

    constexpr uint32_t num_players = 0x8010C3E2;  // s16
    constexpr uint32_t car_list = rush2::car2049::car_list; // u8 [36][2]: car i of player p at + 2 * i + p.
    constexpr uint32_t car_list_size = 0x803CB398; // s16 [2]
    constexpr int num_cars = 22;
    constexpr int max_car_players = 2;

    struct Choice {
        const char* label;
        int8_t value;   // Stored in the cheat's state byte.
        int16_t display; // Index of the label shown in the in-game menu.
    };

    struct Cheat {
        const char* id;
        const char* name;
        const char* description;
        int menu_index;
        uint32_t address;
        uint32_t address2; // Second state byte written alongside the first, 0 if none.
        uint32_t default_choice; // Index of the boot default in choices.
        // Two choices (off, on) with the default first: a checkbox. Otherwise: a selector.
        std::vector<Choice> choices;
    };

    const std::vector<Cheat> cheat_list = {
        { "cheat_no_car_collisions", "No Car Collisions",
            "Cars pass through each other instead of colliding.",
            0, 0x8011249A, 0, 0,
            { { "On", 0, 0 }, { "Off", 1, 1 } } },
        { "cheat_gravity", "Gravity",
            "Changes the strength of gravity, which changes how far cars fly off jumps.",
            1, 0x800E7D22, 0, 1,
            { { "Light", 0, 0 }, { "Normal", 1, 1 }, { "Heavy", 2, 2 }, { "Jovian", 3, 3 } } },
        { "cheat_cone_mines", "Cone Mines",
            "Traffic cones explode when they are hit.",
            2, 0x8010C166, 0, 0,
            { { "Off", 0, 0 }, { "On", 1, 1 } } },
        { "cheat_car_mines", "Car Mines",
            "Cars explode when they hit each other.",
            3, 0x8010C167, 0, 0,
            { { "Off", 0, 0 }, { "On", 1, 1 } } },
        { "cheat_burning_wreck", "Burning Wreck",
            "Your car burns like a wreck while you drive, with or without smoke.",
            4, 0x800D401C, 0x800D401D, 0,
            { { "Off", 0, 0 }, { "With Smoke", 1, 1 }, { "Without Smoke", 2, 2 } } },
        { "cheat_upside_down", "Upside-Down Track",
            "Turns the track upside down.",
            5, 0x800D0171, 0, 0,
            { { "Normal", 0, 0 }, { "Upside-Down", 1, 1 } } },
        { "cheat_auto_abort", "Disable Auto-Abort",
            "Disables automatically putting your car back on the road when it crashes or gets stuck.",
            6, 0x8010C168, 0, 0,
            { { "Off", 0, 0 }, { "On", 1, 1 } } },
        { "cheat_super_speed", "Super Speed",
            "Cars go much faster.",
            7, 0x80111950, 0, 0,
            { { "Off", 0, 0 }, { "On", 1, 1 } } },
        { "cheat_inside_out_car", "Inside-Out Car",
            "Draws your car inside out.",
            8, 0x801174E0, 0, 0,
            { { "Off", 0, 0 }, { "On", 1, 1 } } },
        { "cheat_no_damage", "No Damage",
            "Crashes don't damage cars.",
            9, 0x8011CDB0, 0, 0,
            { { "On", 0, 0 }, { "Off", 1, 1 } } },
        { "cheat_invincible", "Invincible",
            "Your car can't be destroyed.",
            10, 0x80119634, 0, 0,
            { { "Off", 0, 0 }, { "On", 1, 1 } } },
        { "cheat_invisible_car", "Invisible Car",
            "Your car is invisible.",
            11, 0x8010C170, 0, 0,
            { { "Off", 0, 0 }, { "On", 1, 1 } } },
        { "cheat_invisible_track", "Invisible Track",
            "The track is invisible.",
            12, 0x8010C178, 0, 0,
            { { "Off", 0, 0 }, { "On", 1, 1 } } },
        { "cheat_no_brakes", "No Brakes",
            "Cars can't brake.",
            13, 0x80125C64, 0, 0,
            { { "On", 0, 0 }, { "Off", 1, 1 } } },
        { "cheat_super_tires", "Super Tires",
            "Tires grip the road much better.",
            14, 0x8010D3A6, 0, 0,
            { { "Off", 0, 0 }, { "On", 2, 1 } } },
        { "cheat_mass", "Mass",
            "Makes cars heavier, so they push other cars around more easily.",
            15, 0x8010C179, 0, 0,
            { { "Normal", 0, 0 }, { "Heavy", 1, 1 }, { "Massive", 2, 2 } } },
        { "cheat_suicide_mode", "Suicide Mode",
            "Cars drive the track in the opposite direction, into oncoming traffic. "
            "<recomp-color primary>Humans</recomp-color> applies it to human players.",
            16, 0x80119628, 0, 0,
            { { "Off", 0, 0 }, { "On", 1, 1 }, { "Humans", 2, 2 } } },
        { "cheat_do_the_dew", "Do the Dew",
            "Turns on the game's Mountain Dew cheat.",
            17, 0x8010C17A, 0, 0,
            { { "Off", 0, 0 }, { "On", 1, 1 } } },
        { "cheat_killer_rats", "Killer Rats",
            "Rats run across the track.",
            18, 0x8010C17C, 0, 0,
            { { "Off", 0, 0 }, { "On", 1, 1 } } },
        { "cheat_stunts_all_tracks", "Stunts on All Tracks",
            "Lets you score stunts on every track, not just the stunt track.",
            19, 0x8010C200, 0, 0,
            { { "Stunt Track", 0, 0 }, { "All Tracks", 1, 1 } } },
        { "cheat_resurrect_in_place", "Resurrect in Place",
            "After a wreck, your car comes back where it crashed.",
            20, 0x8010C210, 0, 0,
            { { "Off", 0, 0 }, { "On", 1, 1 } } },
        { "cheat_levitation", "Levitation",
            "Cars float above the track.",
            21, 0x8010C254, 0, 0,
            { { "Off", 0, 0 }, { "On", 1, 1 } } },
        { "cheat_no_game_timer", "No Race Timer",
            "Turns off the race time limit.",
            22, 0x800D9E88, 0, 0,
            { { "On", 0, 0 }, { "Off", 1, 1 } } },
        { "cheat_new_york_cabs", "New York Cabs",
            "Replaces the cars with New York taxi cabs.",
            23, 0x800D9E89, 0, 0,
            { { "Off", 0, 0 }, { "On", 1, 1 } } },
        { "cheat_frame_scale", "Frame Scale",
            "Changes the size of the car's body.",
            24, 0x8010C43C, 0, 0,
            { { "0", 0, 0 }, { "1", 1, 1 }, { "2", 2, 2 }, { "3", 3, 3 } } },
        { "cheat_tire_scaling", "Tire Scaling",
            "Changes the size of the car's tires.",
            25, 0x8010C3D4, 0, 0,
            { { "Off", 0, 0 }, { "On", 1, 1 } } },
        { "cheat_fog_color", "Fog Color",
            "Changes the color of the fog.",
            26, 0x8010C440, 0, 0,
            { { "Normal", 0, 0 }, { "Black", 12, 1 }, { "Red", 13, 2 }, { "Green", 14, 3 }, { "Blue", 15, 4 }, { "Yellow", 16, 5 }, { "White", 17, 6 }, { "Orange", 18, 7 }, { "Pink", 19, 8 } } },
    };

    constexpr int no_write = -1;

    // Index into the cheat's choices that the game thread should write next frame, or no_write.
    std::array<std::atomic<int>, num_menu_items> pending_choice;

    // Game thread only: the state byte as of the last frame, and choices changed in game that the config hasn't
    // been updated with yet.
    std::array<int, num_menu_items> last_value;
    std::array<int, num_menu_items> unsynced_choice;
    bool has_unsynced = false;

    const std::string config_id = "cheats";

    const std::string cheat_menu_id = "cheat_menu";
    std::atomic<bool> cheat_menu_enabled = false;
    std::atomic<bool> cheat_menu_restore_pending = false;

    const std::string unlock_tracks_id = "unlock_all_tracks";
    std::atomic<bool> unlock_tracks_enabled = false;
    std::atomic<bool> unlock_tracks_restore_pending = false;

    const std::string unlock_cars_id = "unlock_all_cars";
    std::atomic<bool> unlock_cars_enabled = false;

    const std::string unlock_parts_id = "unlock_all_parts";
    std::atomic<bool> unlock_parts_enabled = false;

    // Not one of the game's: Rush 2049's battle weapons in the other races (src/battle.cpp).
    const std::string weapons_id = "cheat_weapons";

    const std::string unlock_system_id = "unlock_system";
    std::atomic<bool> unlock_system_enabled = true;

    void write_choice(uint8_t* rdram, const Cheat& cheat, const Choice& choice) {
        MEM_B(0, (int32_t)cheat.address) = choice.value;
        if (cheat.address2 != 0) {
            MEM_B(0, (int32_t)cheat.address2) = choice.value;
        }
        MEM_H(2 * cheat.menu_index, (int32_t)cheat_display) = choice.display;
    }

    bool is_checkbox(const Cheat& cheat) {
        return cheat.choices.size() == 2 && cheat.default_choice == 0;
    }

    int find_choice(const Cheat& cheat, int value) {
        for (size_t c = 0; c < cheat.choices.size(); c++) {
            if (cheat.choices[c].value == value) {
                return (int)c;
            }
        }
        return no_write;
    }

    // Copies cheats changed in game into the config. Must not run while a menu is open (see the top of the file).
    void sync_config() {
        recomp::config::Config& config = recompui::config::get_config(config_id);
        for (size_t i = 0; i < cheat_list.size(); i++) {
            int choice = unsynced_choice[i];
            if (choice == no_write) {
                continue;
            }
            unsynced_choice[i] = no_write;
            const Cheat& cheat = cheat_list[i];
            if (is_checkbox(cheat)) {
                config.update_option_value(cheat.id, choice == 1);
            }
            else {
                config.update_option_value(cheat.id, (uint32_t)choice);
            }
        }
        config.save_config();
        has_unsynced = false;
    }

    // Whether a cheat can be used: the Weapons cheat needs the Rush 2049 tracks and Unlock All Parts the Rush 2049
    // cars (a Rush 2049 ROM and their option on).
    bool option_available(const std::string& id) {
        if (id == weapons_id) return rush2::track2049::available();
        if (id == unlock_parts_id) return rush2::car2049::available();
        return true;
    }

    // The tab's page: every option at once, in groups over three columns, with the hovered or focused option's
    // description below them. The config is the frontend's (create_config_tab), whose own tab is hidden.
    struct Group {
        const char* title;
        std::vector<std::string> ids;
    };

    const std::vector<std::vector<Group>> page_columns = {
        {
            { "Unlocks", { cheat_menu_id, unlock_tracks_id, unlock_cars_id, unlock_parts_id } },
            { "Driving", { "cheat_super_speed", "cheat_super_tires", "cheat_no_brakes", "cheat_mass", "cheat_gravity",
                           "cheat_levitation", "cheat_no_car_collisions" } },
        },
        {
            { "Survival", { "cheat_invincible", "cheat_no_damage", "cheat_auto_abort", "cheat_resurrect_in_place",
                            "cheat_no_game_timer" } },
            { "Race", { "cheat_suicide_mode", "cheat_stunts_all_tracks", "cheat_cone_mines", "cheat_car_mines",
                        "cheat_killer_rats", weapons_id } },
        },
        {
            { "Looks", { "cheat_invisible_car", "cheat_invisible_track", "cheat_upside_down", "cheat_inside_out_car",
                         "cheat_burning_wreck", "cheat_frame_scale", "cheat_tire_scaling", "cheat_fog_color",
                         "cheat_new_york_cabs", "cheat_do_the_dew" } },
        },
    };

    const std::string page_description =
        "Cheats apply to every race until they're turned off. Changes made in the game's own Cheats screen show up "
        "here too.";

    // Controls that report when they're hovered or focused, to show their option's description.
    class CheatToggle : public recompui::Toggle {
    public:
        CheatToggle(recompui::ResourceId rid, recompui::Element* parent, std::function<void()> on_focus)
            : Toggle(rid, parent, recompui::ToggleSize::Medium), on_focus(std::move(on_focus)) {}

    protected:
        void process_event(const recompui::Event& e) override {
            Toggle::process_event(e);
            if ((e.type == recompui::EventType::Focus && std::get<recompui::EventFocus>(e.variant).active) ||
                (e.type == recompui::EventType::Hover && std::get<recompui::EventHover>(e.variant).active)) {
                on_focus();
            }
        }

    private:
        std::function<void()> on_focus;
    };

    class CheatSelect : public recompui::Select {
    public:
        CheatSelect(recompui::ResourceId rid, recompui::Element* parent, std::vector<recompui::SelectOption> options,
                    std::string selected, std::function<void()> on_focus)
            : Select(rid, parent, std::move(options), std::move(selected)), on_focus(std::move(on_focus)) {}

    protected:
        void process_event(const recompui::Event& e) override {
            Select::process_event(e);
            if ((e.type == recompui::EventType::Focus && std::get<recompui::EventFocus>(e.variant).active) ||
                (e.type == recompui::EventType::Hover && std::get<recompui::EventHover>(e.variant).active)) {
                on_focus();
            }
        }

    private:
        std::function<void()> on_focus;
    };

    class CheatsPage : public recompui::ConfigPage {
    public:
        CheatsPage(recompui::ResourceId rid, recompui::Element* parent)
            : ConfigPage(rid, parent, recompui::Events(recompui::EventType::Update)) {
            using namespace recompui;
            ContextId context = get_current_context();
            recomp::config::Config& config = recompui::config::get_config(config_id);
            set_as_navigation_container(NavigationType::Vertical);

            // One full-width side: the columns, then the description.
            body->get_right()->set_display(Display::None);
            Element* left = body->get_left();
            left->set_display(Display::Flex);
            left->set_flex_direction(FlexDirection::Column);
            left->set_padding(16.0f);
            left->set_gap(8.0f);

            ConfigHeaderFooter* top = add_header();
            Label* intro = context.create_element<Label>(top->get_left(), page_description, theme::Typography::Body);
            intro->set_color(theme::color::TextDim);
            Button* reset = context.create_element<Button>(top->get_right(), "Reset Cheats", ButtonStyle::Secondary);
            reset->add_pressed_callback([this]() { reset_cheats(); });

            Element* grid = context.create_element<Element>(left, 0, "div", false);
            grid->set_display(Display::Flex);
            grid->set_flex_direction(FlexDirection::Row);
            grid->set_gap(32.0f);
            grid->set_width(100.0f, Unit::Percent);
            grid->set_as_navigation_container(NavigationType::Horizontal);

            for (const auto& groups : page_columns) {
                Element* column = context.create_element<Element>(grid, 0, "div", false);
                column->set_display(Display::Flex);
                column->set_flex_direction(FlexDirection::Column);
                column->set_flex_grow(1.0f);
                column->set_flex_basis(0.0f);
                column->set_as_navigation_container(NavigationType::Vertical);
                for (const Group& group : groups) {
                    Label* title = context.create_element<Label>(column, group.title, theme::Typography::LabelLG);
                    title->set_padding_top(column_has_rows(column) ? 16.0f : 0.0f);
                    title->set_padding_bottom(6.0f);
                    title->set_margin_bottom(4.0f);
                    title->set_border_bottom_width(1.0f);
                    title->set_border_bottom_color(theme::color::Border);
                    for (const std::string& id : group.ids) {
                        add_row(column, config, id);
                    }
                }
            }

            description = context.create_element<Element>(left, 0, "p", true);
            description->set_typography(theme::Typography::Body);
            description->set_line_height(28.0f);
            description->set_padding_top(8.0f);
            description->set_min_height(64.0f);
            description->set_color(theme::color::TextDim);

            queue_update();
        }

    protected:
        std::string_view get_type_name() override { return "CheatsPage"; }

        void process_event(const recompui::Event& e) override {
            if (e.type == recompui::EventType::Update) {
                // Follow changes made elsewhere: the game's Cheats screen, or Reset Cheats.
                recomp::config::Config& config = recompui::config::get_config(config_id);
                config.clear_config_option_updates();
                for (Row& row : rows) {
                    // Cheats that need another game's ROM are grayed out without it.
                    bool enabled = option_available(row.id);
                    if (enabled != row.enabled) {
                        row.enabled = enabled;
                        if (row.toggle != nullptr) row.toggle->set_enabled(enabled);
                        else row.select->set_enabled(enabled);
                    }
                    recomp::config::ConfigValueVariant value = config.get_option_value(row.id);
                    if (value == row.shown) {
                        continue;
                    }
                    row.shown = value;
                    if (row.toggle != nullptr) {
                        row.toggle->set_checked(std::get<bool>(value));
                    }
                    else {
                        row.select->set_selection(std::to_string(std::get<uint32_t>(value)));
                    }
                }
                queue_update();
            }
        }

    private:
        struct Row {
            std::string id;
            recompui::Toggle* toggle = nullptr;
            recompui::Select* select = nullptr;
            recomp::config::ConfigValueVariant shown;
            bool enabled = true;
        };
        std::vector<Row> rows;
        recompui::Element* description = nullptr;
        std::vector<recompui::Element*> columns_with_rows;

        bool column_has_rows(recompui::Element* column) {
            return std::find(columns_with_rows.begin(), columns_with_rows.end(), column) != columns_with_rows.end();
        }

        void add_row(recompui::Element* column, recomp::config::Config& config, const std::string& id) {
            using namespace recompui;
            ContextId context = get_current_context();
            columns_with_rows.push_back(column);
            const recomp::config::ConfigOption& option = config.get_option(id);

            Element* row = context.create_element<Element>(column, 0, "div", false);
            row->set_display(Display::Flex);
            row->set_flex_direction(FlexDirection::Row);
            row->set_align_items(AlignItems::Center);
            row->set_gap(12.0f);
            row->set_height(48.0f);
            row->set_as_navigation_container(NavigationType::Horizontal);
            Label* name = context.create_element<Label>(row, option.name, theme::Typography::LabelMD);
            name->set_flex_grow(1.0f);
            name->set_white_space(WhiteSpace::Nowrap);

            std::function<void()> on_focus = [this, &config, id]() {
                description->set_text_unsafe(config.get_option(id).description);
            };
            Row r{ id };
            r.shown = config.get_option_value(id);
            if (option.type == recomp::config::ConfigOptionType::Bool) {
                CheatToggle* toggle = context.create_element<CheatToggle>(row, on_focus);
                toggle->set_checked(std::get<bool>(r.shown));
                toggle->add_checked_callback([id](bool checked) {
                    recompui::config::get_config(config_id).set_option_value(id, checked);
                });
                r.toggle = toggle;
            }
            else {
                std::vector<SelectOption> options;
                for (const auto& choice : std::get<recomp::config::ConfigOptionEnum>(option.variant).options) {
                    options.emplace_back(choice.name, std::to_string(choice.value));
                }
                // The select fills its parent's width, so it gets a parent of its own.
                Element* holder = context.create_element<Element>(row, 0, "div", false);
                holder->set_width(192.0f);    // The select's minimum width.
                holder->set_flex_shrink(0.0f);
                CheatSelect* select = context.create_element<CheatSelect>(holder, options,
                    std::to_string(std::get<uint32_t>(r.shown)), on_focus);
                select->add_change_callback([id](SelectOption& choice, int) {
                    recompui::config::get_config(config_id).set_option_value(id, (uint32_t)std::stoul(choice.value));
                });
                r.select = select;
            }
            rows.push_back(r);
        }

        // Puts every cheat back to its boot default; the unlocks stay as they are.
        void reset_cheats() {
            recomp::config::Config& config = recompui::config::get_config(config_id);
            for (const Cheat& cheat : cheat_list) {
                if (is_checkbox(cheat)) {
                    config.set_option_value(cheat.id, false);
                }
                else {
                    config.set_option_value(cheat.id, cheat.default_choice);
                }
            }
            config.save_config();
        }
    };
}

void rush2::cheats::create_tab() {
    for (auto& pending : pending_choice) {
        pending = no_write;
    }
    last_value.fill(no_write);
    unsynced_choice.fill(no_write);

    // The frontend's tab for the config lists the options one per row; the Cheats tab shows them in groups instead.
    recomp::config::Config& config = recompui::config::create_config_tab("Cheats", config_id, false);
    recompui::config::set_tab_visible(config_id, false);
    recompui::config::create_tab("Cheats", "rush2_cheats",
        [](recompui::ContextId context, recompui::Element* parent) {
            context.create_element<CheatsPage>(parent);
        },
        nullptr,
        [](recompui::TabCloseContext) {
            recompui::config::get_config(config_id).save_config();
        });

    config.add_bool_option(
        cheat_menu_id,
        "Enable Cheat Menu",
        "Adds the game's hidden <recomp-color primary>Cheats</recomp-color> screen to the Setup menu with every "
        "cheat unlocked, so no button codes are needed.",
        false
    );
    config.add_option_change_callback(cheat_menu_id,
        [](recomp::config::ConfigValueVariant cur_value, recomp::config::ConfigValueVariant, recomp::config::OptionChangeContext) {
            bool enabled = std::get<bool>(cur_value);
            if (cheat_menu_enabled.exchange(enabled) && !enabled) {
                cheat_menu_restore_pending = true;
            }
        });

    config.add_bool_option(
        unlock_tracks_id,
        "Unlock All Tracks",
        "Makes every track available without earning or buying it: <recomp-color primary>Pipe</recomp-color>, "
        "<recomp-color primary>Midway</recomp-color> and the SF Rush and Rush 2049 tracks the unlock system locks. "
        "Nothing is saved, so turning this off locks them again.",
        false
    );
    config.add_option_change_callback(unlock_tracks_id,
        [](recomp::config::ConfigValueVariant cur_value, recomp::config::ConfigValueVariant, recomp::config::OptionChangeContext) {
            bool enabled = std::get<bool>(cur_value);
            if (unlock_tracks_enabled.exchange(enabled) && !enabled) {
                unlock_tracks_restore_pending = true;
            }
        });

    config.add_bool_option(
        unlock_cars_id,
        "Unlock All Cars",
        "Lists every car in car select, Rush 2's mystery cars and the Rush 2049 cars included, without earning or "
        "buying them. Nothing is saved, so turning this off hides them again.",
        false
    );
    config.add_option_change_callback(unlock_cars_id,
        [](recomp::config::ConfigValueVariant cur_value, recomp::config::ConfigValueVariant, recomp::config::OptionChangeContext) {
            unlock_cars_enabled = std::get<bool>(cur_value);
        });

    config.add_bool_option(
        unlock_parts_id,
        "Unlock All Parts",
        "Offers every <recomp-color primary>ENGINE</recomp-color> level on the Rush 2049 cars without buying them. "
        "Nothing is saved, so turning this off locks them again.",
        false
    );
    config.add_option_change_callback(unlock_parts_id,
        [](recomp::config::ConfigValueVariant cur_value, recomp::config::ConfigValueVariant, recomp::config::OptionChangeContext) {
            unlock_parts_enabled = std::get<bool>(cur_value);
        });

    config.add_bool_option(
        unlock_system_id,
        "Unlock System",
        "Keys, Dew cans and coins earn points to buy cars, tracks and parts in the UNLOCKS menu.",
        true
    );
    config.add_option_change_callback(unlock_system_id,
        [](recomp::config::ConfigValueVariant cur_value, recomp::config::ConfigValueVariant, recomp::config::OptionChangeContext) {
            unlock_system_enabled = std::get<bool>(cur_value);
        });

    config.add_enum_option(
        weapons_id,
        "Weapons",
        "Gives every player a Rush 2049 battle weapon in the other races: the one chosen, or with "
        "<recomp-color primary>Random</recomp-color> any of the eight, again a few seconds after it runs out. "
        "<recomp-color primary>Invisibility</recomp-color> gives the battle's power-up instead. Cars have a battle's "
        "health and are wrecked when it runs out. Fire and drop with the <recomp-color primary>FIRE</recomp-color> and "
        "<recomp-color primary>DROP WEAPON</recomp-color> controls. Requires a Rush 2049 (USA) ROM and Rush 2049 Tracks on.",
        {
            { 0u, "Off", "Off" },
            { 1u, "Cannon", "Cannon" },
            { 2u, "Gatling", "Gatling Gun" },
            { 3u, "Grenade", "Grenades" },
            { 4u, "Mine", "Mines" },
            { 5u, "Missile", "Missiles" },
            { 6u, "Ram", "Ram" },
            { 7u, "Rocket", "Rockets" },
            { 8u, "Sonic", "Sonic Blast" },
            { 9u, "Invisibility", "Invisibility" },
            { 10u, "Random", "Random" },
        },
        0u
    );
    config.add_option_change_callback(weapons_id,
        [](recomp::config::ConfigValueVariant cur_value, recomp::config::ConfigValueVariant, recomp::config::OptionChangeContext) {
            rush2::battle::set_weapons_cheat((int)std::get<uint32_t>(cur_value));
        });

    for (size_t i = 0; i < cheat_list.size(); i++) {
        const Cheat& cheat = cheat_list[i];

        if (is_checkbox(cheat)) {
            config.add_bool_option(cheat.id, cheat.name, cheat.description, false);
            config.add_option_change_callback(cheat.id,
                [i](recomp::config::ConfigValueVariant cur_value, recomp::config::ConfigValueVariant, recomp::config::OptionChangeContext) {
                    pending_choice[i] = std::get<bool>(cur_value) ? 1 : 0;
                });
            continue;
        }

        std::vector<recomp::config::ConfigOptionEnumOption> options;
        for (size_t c = 0; c < cheat.choices.size(); c++) {
            options.push_back({ (uint32_t)c, cheat.choices[c].label, cheat.choices[c].label });
        }
        config.add_enum_option(cheat.id, cheat.name, cheat.description, options, cheat.default_choice);
        config.add_option_change_callback(cheat.id,
            [i](recomp::config::ConfigValueVariant cur_value, recomp::config::ConfigValueVariant, recomp::config::OptionChangeContext) {
                pending_choice[i] = (int)std::get<uint32_t>(cur_value);
            });
    }
}

// Top of the game's main loop, once per frame in menus and races.
extern "C" void rush2_cheats_frame(uint8_t* rdram, recomp_context* ctx) {
    if (cheat_menu_enabled.load(std::memory_order_relaxed)) {
        MEM_B(0, (int32_t)cheat_menu_flag) = 1;
        for (int i = 0; i < num_menu_items; i++) {
            MEM_B(i, (int32_t)cheat_unlocked) = 1;
        }
    }
    else if (cheat_menu_restore_pending.exchange(false)) {
        MEM_B(0, (int32_t)cheat_menu_flag) = 0;
        for (int i = 0; i < num_menu_items; i++) {
            MEM_B(i, (int32_t)cheat_unlocked) = 0;
        }
    }

    if (unlock_tracks_enabled.load(std::memory_order_relaxed)) {
        MEM_B(0, (int32_t)pipe_unlocked) = 1;
        MEM_B(0, (int32_t)midway_unlocked) = 1;
    }
    else if (unlock_tracks_restore_pending.exchange(false)) {
        // Recompute them from the save data. Called on a copy of the context, from the top of the main loop.
        recomp_context unlock_ctx = *ctx;
        unlocks_check_tracks_80094F1C(rdram, &unlock_ctx);
    }

    for (size_t i = 0; i < cheat_list.size(); i++) {
        const Cheat& cheat = cheat_list[i];
        int choice = pending_choice[i].exchange(no_write);
        if (choice != no_write) {
            write_choice(rdram, cheat, cheat.choices[choice]);
            // The config already has this value; drop any older in-game change.
            unsynced_choice[i] = no_write;
            last_value[i] = cheat.choices[choice].value;
            continue;
        }

        int value = MEM_B(0, (int32_t)cheat.address);
        if (value != last_value[i]) {
            int changed = find_choice(cheat, value);
            if (last_value[i] != no_write && changed != no_write) {
                unsynced_choice[i] = changed;
                has_unsynced = true;
            }
            last_value[i] = value;
        }
    }

    if (has_unsynced && !recompui::is_any_context_shown()) {
        sync_config();
    }

    // This is the port's once-per-frame hook at the top of the main loop.
    rush2::car2049::prepare_bars(rdram, ctx);
}

bool rush2::cheats::unlock_all_cars() {
    return unlock_cars_enabled.load(std::memory_order_relaxed);
}

bool rush2::cheats::unlock_all_tracks() {
    return unlock_tracks_enabled.load(std::memory_order_relaxed);
}

bool rush2::cheats::unlock_all_parts() {
    return unlock_parts_enabled.load(std::memory_order_relaxed);
}

bool rush2::cheats::unlock_system() {
    return unlock_system_enabled.load(std::memory_order_relaxed);
}

void rush2::cheats::set_unlock_system(bool enabled) {
    recomp::config::Config& config = recompui::config::get_config(config_id);
    config.set_option_value(unlock_system_id, enabled);
    config.save_config();
}

// func_803B81F0 (car select setup) after each player's car list is built: every car with Unlock All Cars, then the
// unlock system's choice (src/unlocks.cpp).
extern "C" void rush2_cheats_car_list(uint8_t* rdram, recomp_context* ctx) {
    int players = std::min<int>(MEM_H(0, (int32_t)num_players), max_car_players);
    for (int p = 0; p < players; p++) {
        if (unlock_cars_enabled.load(std::memory_order_relaxed)) {
            for (int car = 0; car < num_cars; car++) {
                MEM_B(2 * car + p, (int32_t)car_list) = car;
            }
            MEM_H(2 * p, (int32_t)car_list_size) = num_cars;
            rush2::car2049::append_to_car_list(rdram, p);
        }
        rush2::unlocks::filter_car_list(rdram, p);
    }
}
