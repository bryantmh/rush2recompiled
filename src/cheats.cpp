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
// The tab also has two unlocks, which change nothing that is saved:
// - Unlock All Tracks forces the PIPE (0x800E7D50) and MIDWAY (0x800E7D19) availability bytes on every frame. The
//   game recomputes both from the save data in func_80094F1C, which is called again when the option is turned off.
// - Unlock All Cars replaces each player's car list once car select (func_803B81F0) has built it. The game lists
//   cars 0-15, cars 16-19 for every 3 keys found on the current track, car 20 once 0x800C20D8 (or the profile's
//   +0x4B4) is set, and car 21 once all 4 cans on the current track are found.

#include <algorithm>
#include <array>
#include <atomic>
#include <string>
#include <variant>
#include <vector>

#include "recompui/recompui.h"
#include "recompui/config.h"
#include "librecomp/config.hpp"

#include "recomp.h"
#include "rush2_hooks.h"
#include "rush2.h"
#include "car2049.h"

extern "C" void func_80094F1C(uint8_t* rdram, recomp_context* ctx);

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
        { "cheat_auto_abort", "Auto-Abort",
            "Automatically puts your car back on the road when it crashes or gets stuck.",
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
}

void rush2::cheats::create_tab() {
    for (auto& pending : pending_choice) {
        pending = no_write;
    }
    last_value.fill(no_write);
    unsynced_choice.fill(no_write);

    recomp::config::Config& config = recompui::config::create_config_tab("Cheats", config_id, false);

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
        "Makes the <recomp-color primary>Pipe</recomp-color> and <recomp-color primary>Midway</recomp-color> tracks "
        "available without earning them. Nothing is saved, so turning this off locks them again.",
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
        "Lists every car in car select, including the ones unlocked by finding keys and cans on each track. Nothing is "
        "saved, so turning this off hides them again.",
        false
    );
    config.add_option_change_callback(unlock_cars_id,
        [](recomp::config::ConfigValueVariant cur_value, recomp::config::ConfigValueVariant, recomp::config::OptionChangeContext) {
            unlock_cars_enabled = std::get<bool>(cur_value);
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
        func_80094F1C(rdram, &unlock_ctx);
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
}

// func_803B81F0 (car select setup) after each player's car list is built.
extern "C" void rush2_cheats_car_list(uint8_t* rdram, recomp_context* ctx) {
    if (!unlock_cars_enabled.load(std::memory_order_relaxed)) {
        return;
    }
    int players = std::min<int>(MEM_H(0, (int32_t)num_players), max_car_players);
    for (int p = 0; p < players; p++) {
        for (int car = 0; car < num_cars; car++) {
            MEM_B(2 * car + p, (int32_t)car_list) = car;
        }
        MEM_H(2 * p, (int32_t)car_list_size) = num_cars;
        rush2::car2049::append_to_car_list(rdram, p);
    }
}
