// The track and car selects' game filter (include/game_filter.h). Its option is in the General tab (src/config.cpp).
//
// With the tracks (cars) of more than one game on a select, its top right corner, under the header art, shows a
// prompt: the menus' C-up button (Y, Triangle, I on the keyboard; src/controls.cpp get_menu_input) and FILTER.
// Pressing it opens a panel listing those games with ON or OFF; A or left/right turns the game under the cursor on or
// off (the last one on stays on), and B, START or the prompt's button again closes it. While it is open the game sees
// no input (src/input.cpp), and the panel reads the ports itself; closing it keeps every port's buttons from the game
// until they are let go, so the closing press doesn't reach the select.
//
// A changed filter rebuilds the select as the game rebuilds it between visits: its teardown (track select
// func_803AB294(0), car select func_803B81F0(0)) and the rest of the frame skipped, so the next frame sets the screen up
// again with the filtered list (players4's second car select round does the same). The track select keeps the track it
// showed if it is still offered; the car select keeps each player's car, as it opens on the record's car.
//
// The filter applies where the prompt can show: not on a circuit's track select (its TRACK row is greyed), an online
// lobby's selects (each machine shows the host's track), the ghost race's car select (its ghost panel takes the C
// buttons), or the unlock system's shop. A filter that would leave a select empty is ignored there.

// As in src/rush2049/wings.cpp: librecomp's nlohmann::json first.
#include "../lib/N64ModernRuntime/thirdparty/json/json.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <string>
#include <vector>

#include "recomp.h"
#include "librecomp/addresses.hpp"
#include "librecomp/config.hpp"

#include "car2049.h"
#include "car_types.h"
#include "data_files.h"
#include "game_filter.h"
#include "ghost.h"
#include "online.h"
#include "rush2.h"
#include "rush2_hooks.h"
#include "track1.h"
#include "track2049.h"
#include "unlocks.h"
#include "calspeed.h"

extern "C" void text_select_font_80088C24(uint8_t* rdram, recomp_context* ctx);               // Selects a font.
extern "C" void menu_text_set_scale_from_global_80093FA8(uint8_t* rdram, recomp_context* ctx); // Text scale ($f12).
extern "C" void text_select_style_800737E4(uint8_t* rdram, recomp_context* ctx);              // Selects a text style.
extern "C" void text_measure_string_800732AC(uint8_t* rdram, recomp_context* ctx);            // Width of a string.
extern "C" void text_print_string_800734E0(uint8_t* rdram, recomp_context* ctx);              // Prints at (x, y).
extern "C" void menu_play_sound_80064908(uint8_t* rdram, recomp_context* ctx);                // Plays a menu sound.

using namespace rush2::game_filter;

namespace {
    constexpr uint32_t game_state = 0x8010C0D0;       // 1 track select, 2 car select
    constexpr uint32_t menu_pressed = 0x8011962A;     // u16: the menus' presses this frame (enabled ports)
    constexpr uint32_t player_structs = 0x800C2140;   // +1: the player's port
    constexpr uint32_t race_player_count = 0x8010C3E2; // s16: players on the car select (2 per round with 3 or 4)
    constexpr uint16_t n64_a = 0x8000, n64_b = 0x4000, n64_start = 0x1000, n64_up = 0x0800, n64_down = 0x0400,
                       n64_left = 0x0200, n64_right = 0x0100, n64_c_up = 0x0008;

    // Text (the Controller Setup screen's label font and styles, src/controls_menu.cpp).
    constexpr int font_labels = 8;
    constexpr int style_white = 1;
    constexpr int style_gray = 4;
    constexpr int style_green = 0xA;
    constexpr int style_flash = 0x14;

    constexpr int sound_up = 0x3B, sound_down = 0x44, sound_change = 0x49, sound_open = 0x31, sound_close = 0x32;

    // Layout, in the menus' 320 x 240 screen. The prompt sits right-aligned under the header art's right end (about
    // x 306; the game's text drawer clips text past x 308).
    constexpr int prompt_right = 304, prompt_y = 38;
    // The two-player car select's header is narrower and the second player's name box sits under it: the button alone,
    // right of the box and below it (beside the car's name).
    constexpr int compact_x = 288, compact_y = 44;
    constexpr int glyph_w = 16, glyph_h = 14, glyph_y_offset = -4;
    constexpr int panel_x = 80, panel_w = 160, panel_y = 64;
    constexpr int title_y = 8, rows_y = 26, row_h = 14, footer_gap = 8;

    constexpr const char* game_keys[int(Game::Count)] = { "rush2", "sfrush", "rush2049", "calspeed", "custom" };
    constexpr const char* game_names[int(Game::Count)] = { "RUSH 2", "SF RUSH", "RUSH 2049", "CALIFORNIA SPEED",
                                                           "CUSTOM" };

    const std::string option = "game_filter";
    std::atomic<bool> option_on = true;

    // The hidden games per screen (Screen order), as bits; game thread.
    unsigned hidden_mask[2] = {};
    bool loaded = false;

    // The option was turned on or off while games were hidden: the selects rebuild (set up since, built).
    std::atomic<bool> rebuild_wanted[2] = { false, false };
    bool built[2] = {};

    // The panel (game thread; open is read by the input thread).
    std::atomic<bool> panel_open = false;
    Screen panel_screen = Screen::Tracks;
    unsigned panel_hidden = 0;
    std::vector<Game> panel_rows;
    int cursor = 0;
    uint16_t last_buttons = 0;
    int held_dir = 0;
    std::chrono::steady_clock::time_point next_repeat;

    // The car select's games (filter_car_lists), and whether the filter took any car off this setup.
    unsigned car_present = 0;
    bool cars_filtered = false;

    const std::string section = "game_filter";

    // { "tracks": ["rush2049", ...], "cars": [...] }: the hidden games' keys.
    void load() {
        if (loaded) {
            return;
        }
        loaded = true;
        std::string text = rush2::data_files::read(rush2::data_files::File::Saves, section);
        const char* lists[2] = { "\"tracks\"", "\"cars\"" };
        for (int s = 0; s < 2; s++) {
            size_t at = text.find(lists[s]);
            size_t open = at == std::string::npos ? at : text.find('[', at);
            size_t close = open == std::string::npos ? open : text.find(']', open);
            if (close == std::string::npos) {
                continue;
            }
            std::string list = text.substr(open, close - open);
            for (int g = 0; g < int(Game::Count); g++) {
                if (list.find("\"" + std::string(game_keys[g]) + "\"") != std::string::npos) {
                    hidden_mask[s] |= bit(Game(g));
                }
            }
        }
    }

    void save() {
        std::string json = "{";
        const char* lists[2] = { "tracks", "cars" };
        for (int s = 0; s < 2; s++) {
            json += std::string(s ? ", " : " ") + "\"" + lists[s] + "\": [";
            bool first = true;
            for (int g = 0; g < int(Game::Count); g++) {
                if (hidden_mask[s] & bit(Game(g))) {
                    json += std::string(first ? "" : ", ") + "\"" + game_keys[g] + "\"";
                    first = false;
                }
            }
            json += "]";
        }
        json += " }";
        rush2::data_files::write(rush2::data_files::File::Saves, section, json);
    }

    int screen_state(Screen s) {
        return s == Screen::Tracks ? 1 : 2;
    }

    bool applies(unsigned present) {
        return std::popcount(present) >= 2;
    }

    // Calls a game function from a hook, keeping the hooked function's registers. Returns $v0.
    int32_t call(uint8_t* rdram, recomp_context* ctx, void (*func)(uint8_t*, recomp_context*), int32_t a0 = 0,
                 int32_t a1 = 0, int32_t a2 = 0, float f12 = 0.0f) {
        recomp_context saved = *ctx;
        ctx->r4 = a0;
        ctx->r5 = a1;
        ctx->r6 = a2;
        ctx->f12.fl = f12;
        func(rdram, ctx);
        int32_t ret = (int32_t)ctx->r2;
        *ctx = saved;
        return ret;
    }

    // Strings the panel prints, in a scratch buffer of the recomp heap reused each frame.
    uint32_t text_scratch = 0;
    uint32_t text_at = 0;
    constexpr uint32_t text_scratch_size = 0x400;

    uint32_t str(uint8_t* rdram, const std::string& s) {
        if (text_scratch == 0) {
            text_scratch = (uint32_t)((uint8_t*)recomp::alloc(rdram, text_scratch_size) - rdram) + 0x80000000u;
        }
        if (text_at + s.size() + 1 > text_scratch_size) {
            text_at = 0;
        }
        uint32_t at = text_scratch + text_at;
        for (size_t i = 0; i <= s.size(); i++) {
            MEM_B(0, (int32_t)(at + i)) = i < s.size() ? s[i] : 0;
        }
        text_at += (uint32_t)((s.size() + 4) & ~3u);
        return at;
    }

    int width(uint8_t* rdram, recomp_context* ctx, const std::string& s) {
        return call(rdram, ctx, text_measure_string_800732AC, (int32_t)str(rdram, s), -1);
    }

    void print(uint8_t* rdram, recomp_context* ctx, int style, int x, int y, const std::string& s) {
        call(rdram, ctx, text_select_style_800737E4, style);
        call(rdram, ctx, text_print_string_800734E0, x, y, (int32_t)str(rdram, s));
    }

    void close_panel() {
        panel_open = false;
        for (int port = 0; port < rush2::input::num_ports; port++) {
            rush2::input::suppress_until_released(port);
        }
    }

    // Every port's menu buttons, with the stick's up and down as the D-pad's.
    uint16_t read_buttons() {
        uint16_t buttons = 0;
        if (!rush2::input::takes_input()) {
            return buttons;
        }
        for (int port = 0; port < rush2::input::num_ports; port++) {
            if (!rush2::input::port_has_device(port)) {
                continue;
            }
            uint16_t b = 0;
            float x = 0.0f, y = 0.0f;
            rush2::controls::get_menu_input(port, &b, &x, &y);
            b |= y >= 0.5f ? n64_up : y <= -0.5f ? n64_down : 0;
            b |= x >= 0.5f ? n64_right : x <= -0.5f ? n64_left : 0;
            buttons |= b;
        }
        return buttons;
    }

    // Up or down this frame: a new press, or a held one repeating.
    int step(uint16_t buttons) {
        using clock = std::chrono::steady_clock;
        int dir = (buttons & n64_up) ? -1 : (buttons & n64_down) ? 1 : 0;
        auto now = clock::now();
        if (dir != held_dir) {
            held_dir = dir;
            next_repeat = now + std::chrono::milliseconds(400);
            return dir;
        }
        if (dir != 0 && now >= next_repeat) {
            next_repeat = now + std::chrono::milliseconds(110);
            return dir;
        }
        return 0;
    }

    void open_panel(Screen s, unsigned present) {
        panel_screen = s;
        panel_hidden = hidden_mask[int(s)];
        panel_rows.clear();
        for (int g = 0; g < int(Game::Count); g++) {
            if (present & bit(Game(g))) {
                panel_rows.push_back(Game(g));
            }
        }
        cursor = 0;
        last_buttons = read_buttons();
        held_dir = (last_buttons & n64_up) ? -1 : (last_buttons & n64_down) ? 1 : 0;
        next_repeat = std::chrono::steady_clock::now() + std::chrono::milliseconds(400);
        panel_open = true;
    }

    // Turns the cursor's game on or off; the last game shown stays on.
    bool toggle(unsigned present) {
        unsigned g = bit(panel_rows[cursor]);
        if (!(panel_hidden & g) && std::popcount(present & ~(panel_hidden | g)) == 0) {
            return false;
        }
        panel_hidden ^= g;
        return true;
    }
}

void rush2::game_filter::add_option(recomp::config::Config& config) {
    config.add_bool_option(
        option,
        "Game Filter",
        "On the track and car selects, when they offer more than one game's tracks or cars, Y (Triangle, or I on the "
        "keyboard) opens a filter that picks which games' tracks or cars are shown. Off, every game's are shown. Takes "
        "effect at once.",
        true
    );
    config.add_option_change_callback(option,
        [](recomp::config::ConfigValueVariant cur_value, recomp::config::ConfigValueVariant, recomp::config::OptionChangeContext context) {
            bool on = std::get<bool>(cur_value);
            if (option_on.exchange(on) != on && context != recomp::config::OptionChangeContext::Load) {
                // A select already up shows its new list at once.
                rebuild_wanted[0] = true;
                rebuild_wanted[1] = true;
            }
        });
}

const char* rush2::game_filter::option_id() {
    return option.c_str();
}

void rush2::game_filter::apply(bool on) {
    option_on = on;
}

bool rush2::game_filter::enabled() {
    return option_on.load();
}

Game rush2::game_filter::track_game(int t) {
    using namespace rush2::track2049;
    if (t < first_menu_id) return Game::Rush2;
    if (t >= rush2::track1::first_menu_id && t < rush2::track1::first_menu_id + rush2::track1::track_count) return Game::SFRush;
    if (t >= mod_menu_id && t < mod_menu_id + mod_capacity) return Game::Custom;
    if (t >= rush2::calspeed::first_menu_id) return Game::CalSpeed;
    return Game::Rush2049;      // 12-17 and the stunt arenas, obstacle course and battle arenas, 25-37
}

Game rush2::game_filter::car_game(int type) {
    if (type < rush2::car2049::rush2_types) return Game::Rush2;
    return rush2::car_types::is_2049(type) ? Game::Rush2049 : Game::CalSpeed;
}

bool rush2::game_filter::hidden(Screen s, Game g) {
    load();
    return (hidden_mask[int(s)] & bit(g)) != 0;
}

bool rush2::game_filter::frame(uint8_t* rdram, recomp_context* ctx, Screen s, unsigned present) {
    load();
    if (built[int(s)] && rebuild_wanted[int(s)].exchange(false) && hidden_mask[int(s)] != 0) {
        if (panel_open && panel_screen == s) close_panel();
        return true;
    }
    if (!enabled() || !applies(present)) {
        if (panel_open && panel_screen == s) close_panel();
        return false;
    }
    if (!panel_open) {
        if (MEM_HU(0, (int32_t)menu_pressed) & n64_c_up) {
            open_panel(s, present);
            call(rdram, ctx, menu_play_sound_80064908, sound_open);
        }
        return false;
    }
    if (panel_screen != s || panel_rows.empty()) {
        return false;
    }

    uint16_t buttons = read_buttons();
    uint16_t pressed = buttons & ~last_buttons;
    last_buttons = buttons;
    int dir = step(buttons);
    if (dir != 0) {
        cursor = (cursor + dir + (int)panel_rows.size()) % (int)panel_rows.size();
        call(rdram, ctx, menu_play_sound_80064908, dir < 0 ? sound_up : sound_down);
    }
    if ((pressed & (n64_a | n64_left | n64_right)) && toggle(present)) {
        call(rdram, ctx, menu_play_sound_80064908, sound_change);
    }
    if (pressed & (n64_b | n64_start | n64_c_up)) {
        close_panel();
        call(rdram, ctx, menu_play_sound_80064908, sound_close);
        if (panel_hidden != hidden_mask[int(s)]) {
            hidden_mask[int(s)] = panel_hidden;
            save();
            return true;
        }
    }
    return false;
}

void rush2::game_filter::draw(uint8_t* rdram, recomp_context* ctx, Screen s, unsigned present) {
    if (!enabled() || !applies(present)) {
        return;
    }
    using rush2::controls::MenuButton;
    int port = MEM_BU(1, (int32_t)player_structs);
    text_at = 0;
    call(rdram, ctx, text_select_font_80088C24, font_labels);
    call(rdram, ctx, menu_text_set_scale_from_global_80093FA8, 0, 0, 0, 0.0f);

    bool open = panel_open && panel_screen == s;
    if (open) {
        int rows = (int)panel_rows.size();
        int footer_y = panel_y + rows_y + rows * row_h + footer_gap;
        int bottom = footer_y + glyph_h + 2;
        // The box: black, translucent, with a green frame like the menus' highlights.
        rush2::controls::begin_menu_glyphs(rdram);
        rush2::controls::add_menu_rect(rdram, panel_x, panel_y, panel_w, bottom - panel_y, 0x000000D8);
        constexpr uint32_t frame_color = 0x30C050FF;
        rush2::controls::add_menu_rect(rdram, panel_x, panel_y, panel_w, 1, frame_color);
        rush2::controls::add_menu_rect(rdram, panel_x, bottom - 1, panel_w, 1, frame_color);
        rush2::controls::add_menu_rect(rdram, panel_x, panel_y, 1, bottom - panel_y, frame_color);
        rush2::controls::add_menu_rect(rdram, panel_x + panel_w - 1, panel_y, 1, bottom - panel_y, frame_color);
        rush2::controls::end_menu_glyphs(rdram);

        std::string title = s == Screen::Tracks ? "SHOW TRACKS FROM" : "SHOW CARS FROM";
        print(rdram, ctx, style_green, panel_x + (panel_w - width(rdram, ctx, title)) / 2, panel_y + title_y, title);
        for (int r = 0; r < rows; r++) {
            int y = panel_y + rows_y + r * row_h;
            bool shown = !(panel_hidden & bit(panel_rows[r]));
            print(rdram, ctx, r == cursor ? style_flash : style_gray, panel_x + 10, y, game_names[int(panel_rows[r])]);
            std::string value = shown ? "ON" : "OFF";
            print(rdram, ctx, shown ? style_green : style_gray, panel_x + panel_w - 10 - width(rdram, ctx, value), y,
                  value);
        }
        // A TOGGLE   B DONE, centered under the rows.
        const std::string toggle_text = "TOGGLE", done_text = "DONE";
        int total = glyph_w + 3 + width(rdram, ctx, toggle_text) + 14 + glyph_w + 3 + width(rdram, ctx, done_text);
        int x = panel_x + (panel_w - total) / 2;
        int text_y = footer_y - glyph_y_offset;
        print(rdram, ctx, style_white, x + glyph_w + 3, text_y, toggle_text);
        int done_x = x + glyph_w + 3 + width(rdram, ctx, toggle_text) + 14;
        print(rdram, ctx, style_white, done_x + glyph_w + 3, text_y, done_text);
        rush2::controls::begin_menu_glyphs(rdram);
        rush2::controls::add_menu_glyph(rdram, port, MenuButton::A, x, footer_y, glyph_w, glyph_h);
        rush2::controls::add_menu_glyph(rdram, port, MenuButton::B, done_x, footer_y, glyph_w, glyph_h);
        rush2::controls::end_menu_glyphs(rdram);
        return;
    }

    if (s == Screen::Cars && (int16_t)MEM_H(0, (int32_t)race_player_count) >= 2) {
        rush2::controls::begin_menu_glyphs(rdram);
        rush2::controls::add_menu_glyph(rdram, port, MenuButton::Y, compact_x, compact_y, glyph_w, glyph_h);
        rush2::controls::end_menu_glyphs(rdram);
        return;
    }
    // The prompt: the button and FILTER, under the header art's right end.
    const std::string label = "FILTER";
    int text_x = prompt_right - width(rdram, ctx, label);
    print(rdram, ctx, style_white, text_x, prompt_y, label);
    rush2::controls::begin_menu_glyphs(rdram);
    rush2::controls::add_menu_glyph(rdram, port, MenuButton::Y, text_x - glyph_w - 3, prompt_y + glyph_y_offset, glyph_w,
                                    glyph_h);
    rush2::controls::end_menu_glyphs(rdram);
}

void rush2::game_filter::screen_built(Screen s) {
    built[int(s)] = true;
    rebuild_wanted[int(s)] = false;
}

bool rush2::game_filter::input_captured() {
    return panel_open.load(std::memory_order_relaxed);
}

void rush2::game_filter::game_frame(uint8_t* rdram) {
    int state = MEM_W(0, (int32_t)game_state);
    for (Screen s : { Screen::Tracks, Screen::Cars }) {
        if (state != screen_state(s)) {
            built[int(s)] = false;
            if (panel_open && panel_screen == s) close_panel();
        }
    }
}

void rush2::game_filter::filter_car_lists(uint8_t* rdram, int players) {
    using rush2::car2049::car_list;
    constexpr uint32_t car_list_size = 0x803CB398;  // s16 per player
    load();
    screen_built(Screen::Cars);
    cars_filtered = false;
    car_present = 0;

    std::vector<std::vector<int>> lists(players);
    unsigned present = 0;
    for (int p = 0; p < players; p++) {
        int n = (int16_t)MEM_H(0, (int32_t)(car_list_size + p * 2));
        for (int i = 0; i < n && i < rush2::car2049::types; i++) {
            int type = (int8_t)MEM_B(0, (int32_t)(car_list + i * 2 + p));
            lists[p].push_back(type);
            present |= bit(car_game(type));
        }
    }
    if (!applies(present) || rush2::ghost::chosen() || rush2::online::game::in_lobby() ||
        rush2::unlocks::shop_car() >= 0) {
        return;
    }
    car_present = present;
    if (!enabled()) {
        return;
    }
    for (int p = 0; p < players; p++) {
        std::vector<int> kept;
        for (int type : lists[p]) {
            if (!hidden(Screen::Cars, car_game(type))) kept.push_back(type);
        }
        if (kept.empty() || kept.size() == lists[p].size()) {
            continue;
        }
        for (size_t i = 0; i < kept.size(); i++) {
            MEM_B(0, (int32_t)(car_list + i * 2 + p)) = (int8_t)kept[i];
        }
        MEM_H(0, (int32_t)(car_list_size + p * 2)) = (int16_t)kept.size();
        cars_filtered = true;
    }
}

// The car select's frame (func_803B9478 at 0x803B94C0, before it sets itself up for the frame with func_803B81F0(1)).
// A changed filter tears the screen down and skips the rest of the frame (goto its end).
extern "C" void carselect_build_list_803B81F0(uint8_t* rdram, recomp_context* ctx);
extern "C" int rush2_filter_car_frame(uint8_t* rdram, recomp_context* ctx) {
    if (!rush2::game_filter::frame(rdram, ctx, Screen::Cars, car_present)) {
        return 0;
    }
    call(rdram, ctx, carselect_build_list_803B81F0, 0);
    return 1;
}

// End of the car select's text callback (func_803BC048 at 0x803BC7AC): the prompt and the panel. The unlock system's
// shop draws its text with this callback too.
extern "C" void rush2_filter_car_text(uint8_t* rdram, recomp_context* ctx) {
    if (MEM_W(0, (int32_t)game_state) != 2 || rush2::unlocks::shop_car() >= 0) {
        return;
    }
    rush2::game_filter::draw(rdram, ctx, Screen::Cars, car_present);
}

// func_803B81F0 at 0x803B8CE4, after rush2_car49_select_cursor: $v0 = the car type the carousel opens on for the player
// at sp + 0x104 (the record's), looked up in the player's list. A car the filter took off would not be found, and the
// game would then open on index 0 with the type set to the player number; open on the list's first car instead.
extern "C" void rush2_filter_car_cursor(uint8_t* rdram, recomp_context* ctx) {
    if (!cars_filtered) {
        return;
    }
    using rush2::car2049::car_list;
    constexpr uint32_t car_list_size = 0x803CB398;
    int player = (int32_t)MEM_W(0x104, (int32_t)ctx->r29);
    if (player < 0 || player > 1) {
        return;
    }
    int n = (int16_t)MEM_H(0, (int32_t)(car_list_size + player * 2));
    int type = (int8_t)(ctx->r2 & 0xFF);
    for (int i = 0; i < n; i++) {
        if ((int8_t)MEM_B(0, (int32_t)(car_list + i * 2 + player)) == type) {
            return;
        }
    }
    if (n > 0) {
        ctx->r2 = (uint64_t)(int64_t)(int8_t)MEM_B(0, (int32_t)(car_list + player));
    }
}
