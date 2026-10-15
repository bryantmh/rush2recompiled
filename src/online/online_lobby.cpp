// Online lobbies in the game's own menus (docs/online.md#the-lobby): the host goes through Start Game, Select Player,
// the track select and the car select as in a local race, and every other player's game follows it.
//
// - Lobby: while the host is anywhere else in its menus, the others wait on the title screen (its attract demo held
//   off) with their controllers off.
// - Select Player: when the host chooses One Race, Stunt or Battle on Start Game, everyone's game goes there too
//   (START on the title screen, the same choice on Start Game) and each player picks their own profile. The host is
//   never held: it goes on to its track select, which only takes its confirm once everyone is on theirs.
// - Track select: the host's. The others' track selects show the host's every frame (track, carousel, options and
//   cursor; their controllers are off) and confirm when the host's does, so they go on to the car select with it.
// - Car select: everyone picks their own car. A player who has stays on the screen, controller off
//   (car_select_done), until all have; then the host starts the race and every car select starts it.
// Each machine's progress comes from where its own game is, never from what it was told, so a player who backs out
// or joins late is waited for and nobody waits on a step that already happened.
// The game's controller reads go through controller_override (main.cpp): ports 2-4 are off throughout, so nobody
// else joins on the same machine, and port 1 is the player's, masked, off, or pressed for the player.
//
// The race the host chose (race type, track, the track select's settings) goes to every machine with its progress
// (publish_menu) and sets the race up (online_game.cpp); the menus are only how the players get there.

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include "recomp.h"
#include "recompui/recompui.h"
#include "elements/ui_label.h"
#include "elements/ui_element.h"

#include "online.h"
#include "battle.h"
#include "collectibles.h"
#include "track2049.h"

namespace {
    using namespace rush2::online;
    using rush2::track2049::StartKind;

    // Game variables.
    constexpr uint32_t game_state = 0x8010C0D0;      // 0 front end, 1 track select, 2 car select, 7+ the race
    constexpr uint32_t front_end_screen = 0x800E7BC4;      // s16, -1 none (func_800AE0D4's screens)
    constexpr uint32_t front_end_next_screen = 0x800E7BB4; // s32: func_800AE288 switches to it
    constexpr int screen_start_game = 0;
    constexpr int screen_select_player = 1;
    constexpr int menu_screens_end = 0xE;            // 0-0xD the menus; 0xE-0x12 the logos, title and attract demo
    constexpr int screen_title = 0x10;
    constexpr uint32_t attract_start = 0x8010C034;   // f32: when the title screen came up (its attract demo timer)
    constexpr uint32_t game_time = 0x80117488;       // f32
    // u8: in the front end, 2 leaves it for the track select (Select Player); nonzero while the attract demo runs.
    constexpr uint32_t front_end_exit = 0x800FAE6C;
    constexpr uint32_t start_game_row = 0x80023064;  // u16: the Start Game menu's row (func_803B12C8)
    constexpr uint32_t race_type = 0x8010C3E8;       // s32: 0 a race, 1 circuit, 2 practice (Start Game)
    constexpr uint32_t track_id = 0x8010C3F0;        // u8: the track select id
    constexpr uint32_t menu_settings = 0x800D5760;   // the track select's settings (laps, drones, options)
    constexpr uint32_t car_table = 0x800D9C10;       // 9 bytes per car slot: +1 type, +2-+5 colors and stripe
    constexpr uint32_t player_records = 0x800C2140;  // 0x28 each: +0x24 the save profile
    constexpr int32_t record_profile = 0x24;
    constexpr uint32_t profile_size = 0x6C0;
    // The track select (func_803ABE0C; src/rush2049/track2049_menu.cpp): the carousel entry shown (s16), the option
    // row under the cursor (s32), the top option row on screen (s32) and the option list's length (s32).
    constexpr uint32_t carousel_shown = 0x803D05B4;
    constexpr uint32_t option_cursor = 0x803D05BC;
    constexpr uint32_t option_top = 0x803D05C4;
    constexpr uint32_t option_count = 0x803D05CC;

    constexpr uint16_t button_a = 0x8000, button_b = 0x4000, button_start = 0x1000;
    constexpr int settle_frames = 20;  // On the track select this long before it takes a confirm.

    // How the game's controller ports read (controller_override, called from the threads that read them).
    enum class Ports : int {
        Off,        // Not online: the controllers as they are.
        Own,        // The player's controller on port 1.
        NoConfirm,  // The player's, without A and START.
        BackOnly,   // The player's B only.
        Blocked,    // Nothing.
        Fed,        // Buttons pressed for the player (fed_buttons).
    };
    std::atomic<int> ports_mode = (int)Ports::Off;
    std::atomic<uint16_t> fed_buttons = 0;

    void set_ports(Ports mode, uint16_t buttons = 0) {
        fed_buttons = buttons;
        ports_mode = (int)mode;
    }

    // This machine's place in the lobby.
    bool car_chosen = false;       // Picked a car on the car select (car_select_done).
    bool start_sent = false;       // Host: asked for the race to start.
    bool allow_back = false;       // Backing out of the car select is how this machine follows the host back.
    bool after_race = false;       // Back to the track select once the post-race is done.
    int frames_in_state = 0;
    int last_state = -1;
    int pulse = 0;
    std::string notice;            // What the lobby waits for, for the overlay.
    std::string race_waiting;      // In a race: who it waits for (race_notice).

    // The battle rules the host's track select shows, copied onto this machine's while it follows (they are this
    // machine's saved settings otherwise: put back after).
    bool following_rules = false;
    int own_points = 0, own_minutes = 0;
    bool own_race_battle = false;

    int32_t state_of(uint8_t* rdram) { return MEM_W(0, (int32_t)game_state); }
    int screen_of(uint8_t* rdram) { return MEM_H(0, (int32_t)front_end_screen); }

    // A button pressed for 2 frames out of every `period`.
    uint16_t pulsed(uint16_t button, int period = 8) {
        return pulse % period < 2 ? button : 0;
    }

    bool supported(StartKind kind) {
        return kind == StartKind::OneRace || kind == StartKind::Stunt || kind == StartKind::Battle;
    }

    const char* kind_name(StartKind kind) {
        switch (kind) {
            case StartKind::Stunt: return "Stunt";
            case StartKind::Battle: return "Battle";
            default: return "One Race";
        }
    }

    StartKind own_start_kind(uint8_t* rdram) {
        return rush2::track2049::start_menu_kind(MEM_HU(0, (int32_t)start_game_row));
    }

    RaceSettings menu_settings_now(uint8_t* rdram) {
        RaceSettings s;
        s.race_type = (uint8_t)MEM_W(0, (int32_t)race_type);
        s.start_kind = (uint8_t)own_start_kind(rdram);
        s.track = (uint8_t)MEM_BU(0, (int32_t)track_id);
        for (int i = 0; i < (int)s.menu.size(); i++) {
            s.menu[i] = (uint8_t)MEM_BU(i, (int32_t)menu_settings);
        }
        // Track select ids (src/rush2049/track2049.h, src/rush1/track1.h): 11 Rush 2's stunt arena, 12-17 Rush 2049's
        // tracks, 18-24 SF Rush's, 25-28 Rush 2049's stunt arenas, 29 its obstacle course, 30-37 its battle arenas.
        int t = s.track;
        s.mode = t == 11 || (t >= 25 && t <= 28) ? GameMode::Stunt : t >= 30 && t <= 37 ? GameMode::Battle : GameMode::Race;
        s.needs_games = (t >= 12 && t <= 17) || (t >= 25 && t <= 37) ? games_rush2049 : (t >= 18 && t <= 24) ? games_rush1 : 0;
        s.battle_points = (uint8_t)rush2::battle::points_to_win();
        s.battle_minutes = (uint8_t)rush2::battle::time_limit_minutes();
        s.race_battle = rush2::battle::race_battle() ? 1 : 0;
        return s;
    }

    // Keeps the front end on the title screen: from a menu it goes there, and the title doesn't go on to its attract
    // demo (func_800AE288 starts it once the title has been up a while).
    void wait_on_title(uint8_t* rdram) {
        int screen = screen_of(rdram);
        int next = MEM_W(0, (int32_t)front_end_next_screen);
        if (screen >= 0 && screen < menu_screens_end && screen == next) {
            MEM_W(0, (int32_t)front_end_next_screen) = screen_title;
        }
        if (screen == screen_title) {
            MEM_W(0, (int32_t)attract_start) = MEM_W(0, (int32_t)game_time);
        }
    }

    // The other connected players below progress `below`, by name.
    std::string list_names(const std::vector<RosterEntry>& r, int me, Progress below) {
        std::string names;
        for (int i = 0; i < (int)r.size(); i++) {
            if (i != me && r[i].used && r[i].connected && r[i].progress < uint8_t(below)) {
                names += (names.empty() ? "" : ", ") + r[i].name;
            }
        }
        return names;
    }

    bool all_at_least(const std::vector<RosterEntry>& r, int me, Progress p) {
        return list_names(r, me, p).empty();
    }

    // The other connected players without the game ROMs `games` (games_* bits).
    std::string missing_games(const std::vector<RosterEntry>& r, int me, uint8_t games) {
        std::string names;
        for (int i = 0; i < (int)r.size(); i++) {
            if (i != me && r[i].used && r[i].connected && (r[i].games & games) != games) {
                names += (names.empty() ? "" : ", ") + r[i].name;
            }
        }
        return names;
    }

    void stop_following_rules() {
        if (following_rules) {
            rush2::battle::set_points_to_win(own_points);
            rush2::battle::set_time_limit_minutes(own_minutes);
            rush2::battle::set_race_battle(own_race_battle);
            following_rules = false;
        }
    }

    // Shows the host's track select on this one: the track (and the carousel entry showing it), the settings, the
    // option cursor and the battle rules.
    void follow_track_select(uint8_t* rdram, const MenuState& hm) {
        const RaceSettings& s = hm.settings;
        MEM_B(0, (int32_t)track_id) = (int8_t)s.track;
        int index = rush2::track2049::carousel_index(rdram, s.track);
        if (index >= 0) {
            MEM_H(0, (int32_t)carousel_shown) = (int16_t)index;
        }
        for (int i = 0; i < (int)s.menu.size(); i++) {
            MEM_B(i, (int32_t)menu_settings) = (int8_t)s.menu[i];
        }
        int count = MEM_W(0, (int32_t)option_count);
        MEM_W(0, (int32_t)option_cursor) = std::clamp<int>(hm.option_cursor, 0, std::max(count - 1, 0));
        MEM_W(0, (int32_t)option_top) = std::clamp<int>(hm.option_top, 1, std::max(count - 1, 1));
        if (!following_rules) {
            own_points = rush2::battle::points_to_win();
            own_minutes = rush2::battle::time_limit_minutes();
            own_race_battle = rush2::battle::race_battle();
            following_rules = true;
        }
        rush2::battle::set_points_to_win(s.battle_points);
        rush2::battle::set_time_limit_minutes(s.battle_minutes);
        rush2::battle::set_race_battle(s.race_battle != 0);
    }

    void host_frame(uint8_t* rdram) {
        int gs = state_of(rdram);
        MenuPhase phase = gs == 1 ? MenuPhase::Track : gs == 2 ? MenuPhase::Car :
            (gs == 0 && screen_of(rdram) == screen_select_player) ? MenuPhase::SelectPlayer : MenuPhase::Lobby;
        std::vector<RosterEntry> r = roster();
        int me = local_slot();
        RaceSettings s = menu_settings_now(rdram);
        bool others = false;
        for (int i = 0; i < (int)r.size(); i++) {
            others |= i != me && r[i].used && r[i].connected;
        }
        // Circuit, Practice, Ghost Race and the profile screens (Records, Unlocks) aren't raced online: the others
        // stay in the lobby.
        bool unsupported = phase != MenuPhase::Lobby && !supported(StartKind(s.start_kind));

        if (phase != MenuPhase::Car) {
            car_chosen = false;
            start_sent = false;
        }
        set_progress(phase == MenuPhase::Car ? (car_chosen ? Progress::CarChosen : Progress::ChoosingCar) :
            phase == MenuPhase::Track ? Progress::TrackSelect :
            phase == MenuPhase::SelectPlayer ? Progress::ChoosingPlayer : Progress::Lobby);

        bool all_on_track = all_at_least(r, me, Progress::TrackSelect);
        std::string lacking = missing_games(r, me, s.needs_games);
        bool can_confirm = !unsupported && all_on_track && lacking.empty() && frames_in_state >= settle_frames;

        if (gs == 1) {
            // The host browses its track select freely but confirms only once everyone is on theirs.
            set_ports(can_confirm || !others ? Ports::Own : Ports::NoConfirm);
        }
        else if (gs == 2 && car_chosen) {
            // B still takes everyone back to the track select, as it does in a local race.
            set_ports(start_sent ? Ports::Blocked : Ports::BackOnly);
        }
        else {
            set_ports(Ports::Own);
        }

        MenuState m;
        m.phase = unsupported ? MenuPhase::Lobby : phase;
        m.settings = s;
        m.option_cursor = (int8_t)MEM_W(0, (int32_t)option_cursor);
        m.option_top = (int8_t)MEM_W(0, (int32_t)option_top);
        publish_menu(m);

        if (unsupported && others) {
            notice = "Online races are One Race, Stunt and Battle: back out to Start Game and pick one of those.";
        }
        else if (phase == MenuPhase::Track && !lacking.empty()) {
            notice = lacking + (lacking.find(',') == std::string::npos ? " doesn't" : " don't") + " have the " +
                (s.needs_games == games_rush1 ? "SF Rush" : "Rush 2049") + " ROM this track needs.";
        }
        else if (phase == MenuPhase::Track && !all_on_track) {
            notice = "Waiting for " + list_names(r, me, Progress::TrackSelect) + " to get to the track select.";
        }
        else if (phase == MenuPhase::Car && car_chosen && !all_at_least(r, me, Progress::CarChosen)) {
            notice = "Waiting for " + list_names(r, me, Progress::CarChosen) + " to pick a car.";
        }
        else if (phase == MenuPhase::Lobby) {
            notice = others ? "Start Game > One Race, Stunt or Battle takes everyone to Select Player." :
                "Waiting for players to join.";
        }
        else {
            notice.clear();
        }

        if (phase == MenuPhase::Car && car_chosen && !start_sent && all_at_least(r, me, Progress::CarChosen)) {
            start_sent = start_race(s);
            if (!start_sent) {
                notice = last_error();
            }
        }
    }

    void joiner_frame(uint8_t* rdram) {
        MenuState hm;
        if (!host_menu(&hm)) {
            hm.phase = MenuPhase::Lobby;
        }
        int gs = state_of(rdram);
        int screen = screen_of(rdram);
        std::vector<RosterEntry> r = roster();
        std::string host = r.empty() || r[0].name.empty() ? "the host" : r[0].name;
        StartKind kind = StartKind(hm.settings.start_kind);
        allow_back = false;
        if (gs != 2) {
            car_chosen = false;
        }
        if (gs != 1) {
            stop_following_rules();
        }

        if (hm.phase == MenuPhase::Lobby) {
            if (gs == 1 || gs == 2) {
                // The host left the race menus: back out to the front end.
                allow_back = true;
                set_ports(Ports::Fed, pulsed(button_b, 4));
            }
            else if (screen >= 0 && screen < menu_screens_end && screen != screen_start_game) {
                // Back from Select Player and the like to Start Game, then to the title screen.
                set_ports(Ports::Fed, pulsed(button_b));
            }
            else {
                wait_on_title(rdram);
                set_ports(Ports::Blocked);
            }
            set_progress(Progress::Lobby);
            notice = "Waiting for " + host + " to start a race.";
            return;
        }

        if (gs == 0) {
            // Everyone picks their own player first, getting there as the host did: START on the title screen (which
            // makes this machine's controller player 1, func_800A6B58), the host's choice on Start Game (its row
            // differs by machine), then Select Player.
            int row = rush2::track2049::start_menu_row(kind);
            if (screen == screen_select_player) {
                set_ports(Ports::Own);
                notice = "Choose a player.";
            }
            else if (screen == screen_start_game && row < 0) {
                set_ports(Ports::Blocked);
                notice = std::string("Your game has no ") + kind_name(kind) + " (it needs the Rush 2049 ROM).";
            }
            else if (screen == screen_start_game) {
                MEM_H(0, (int32_t)start_game_row) = (int16_t)row;
                set_ports(Ports::Fed, pulsed(button_a));
                notice.clear();
            }
            else if (screen >= menu_screens_end) {
                set_ports(Ports::Fed, pulsed(button_start));
                notice.clear();
            }
            else if (screen < 0) {
                set_ports(Ports::Blocked);
                notice.clear();
            }
            else {
                // A screen Select Player opened (a new profile's name, say): the player's own.
                set_ports(Ports::Own);
                notice = "Choose a player.";
            }
            set_progress(Progress::ChoosingPlayer);
            return;
        }

        if (gs == 1) {
            if (own_start_kind(rdram) != kind) {
                // This track select is for another kind of race than the host's (the host went back and chose
                // another): back out and come in again.
                set_ports(Ports::Fed, pulsed(button_b, 4));
                set_progress(Progress::Lobby);
                notice.clear();
                return;
            }
            follow_track_select(rdram, hm);
            if (hm.phase == MenuPhase::Car && frames_in_state >= settle_frames) {
                // The host went on to the car select: so does this one.
                set_ports(Ports::Fed, pulsed(button_a, 4));
                notice.clear();
            }
            else {
                set_ports(Ports::Blocked);
                notice = hm.phase == MenuPhase::Track ? host + " is choosing the track." :
                    hm.phase == MenuPhase::Car ? std::string() : "Waiting for " + host + ".";
            }
            set_progress(Progress::TrackSelect);
            return;
        }

        if (gs == 2) {
            if (hm.phase != MenuPhase::Car) {
                // The host went back to the track select.
                allow_back = true;
                set_ports(Ports::Fed, pulsed(button_b, 4));
                set_progress(Progress::ChoosingCar);
                notice.clear();
                return;
            }
            set_ports(car_chosen ? Ports::Blocked : Ports::Own);
            set_progress(car_chosen ? Progress::CarChosen : Progress::ChoosingCar);
            notice = !car_chosen ? "Pick your car." : all_at_least(r, local_slot(), Progress::CarChosen) ?
                "Waiting for " + host + " to start the race." :
                "Waiting for " + list_names(r, local_slot(), Progress::CarChosen) + " to pick a car.";
            return;
        }
    }

    // The overlay: the lobby's players and what it waits for, over the game. It takes no input, and steps aside
    // whenever a menu or prompt is up (showing a document focuses it).
    struct Overlay {
        recompui::ContextId context = recompui::ContextId::null();
        recompui::Label* title = nullptr;
        recompui::Label* lines[max_players] = {};
        recompui::Label* status = nullptr;
        bool shown = false;
        std::string shown_text;
    };
    Overlay overlay;
    std::mutex overlay_mutex;

    const char* progress_text(uint8_t p) {
        switch (Progress(p)) {
            case Progress::Lobby: return "in the lobby";
            case Progress::ChoosingPlayer: return "choosing a player";
            case Progress::TrackSelect: return "at the track select";
            case Progress::ChoosingCar: return "picking a car";
            case Progress::CarChosen: return "ready";
        }
        return "";
    }

    void update_overlay(bool show) {
        std::lock_guard lock{ overlay_mutex };
        if (overlay.context == recompui::ContextId::null()) {
            return;
        }
        // The overlay itself never captures input, so this is about the other contexts.
        show = show && !recompui::is_context_capturing_input() && !recompui::is_prompt_open();
        if (!show) {
            if (overlay.shown) {
                recompui::hide_context(overlay.context);
                overlay.shown = false;
            }
            return;
        }
        std::vector<RosterEntry> r = roster();
        int me = local_slot();
        std::string text = lobby_name() + "|" + notice;
        std::string lines[max_players];
        for (int i = 0; i < max_players && i < (int)r.size(); i++) {
            if (r[i].used) {
                lines[i] = r[i].name + (i == me ? " (you)" : "") + "  -  " +
                    (r[i].connected ? progress_text(r[i].progress) : "left") +
                    (i != me && r[i].connected && i != 0 && is_host() ? "  " + std::to_string(r[i].ping_ms) + " ms" : "");
            }
            text += "|" + lines[i];
        }
        if (text != overlay.shown_text) {
            overlay.shown_text = text;
            overlay.context.open();
            overlay.title->set_text("Online Lobby: " + lobby_name());
            for (int i = 0; i < max_players; i++) {
                overlay.lines[i]->set_text(lines[i]);
                overlay.lines[i]->set_display(lines[i].empty() ? recompui::Display::None : recompui::Display::Block);
            }
            overlay.status->set_text(notice);
            overlay.status->set_display(notice.empty() ? recompui::Display::None : recompui::Display::Block);
            overlay.context.close();
        }
        if (!overlay.shown) {
            recompui::show_context(overlay.context, "");
            overlay.shown = true;
        }
    }
}

void rush2::online::game::init_overlay() {
    using namespace recompui;
    std::lock_guard lock{ overlay_mutex };
    // Called while the launcher builds its menu, with its context open: one context is open at a time.
    ContextId previous = try_close_current_context();
    ContextId context = create_context();
    context.open();
    context.set_captures_input(false);
    context.set_captures_mouse(false);
    // As recompui's prompts lay theirs out: a window, a wrapper filling it, the panel in its top left corner.
    Element* window = context.create_element<Element>(context.get_root_element());
    window->set_display(Display::Flex);
    window->set_flex_direction(FlexDirection::Column);
    window->set_background_color(theme::color::Transparent);
    Element* wrapper = context.create_element<Element>(window);
    wrapper->set_position(Position::Absolute);
    wrapper->set_top(0);
    wrapper->set_right(0);
    wrapper->set_bottom(0);
    wrapper->set_left(0);
    wrapper->set_display(Display::Flex);
    wrapper->set_flex_direction(FlexDirection::Column);
    wrapper->set_align_items(AlignItems::FlexStart);
    wrapper->set_justify_content(JustifyContent::FlexStart);
    wrapper->set_padding(16.0f, Unit::Dp);
    Element* panel = context.create_element<Element>(wrapper);
    panel->set_display(Display::Flex);
    panel->set_flex_direction(FlexDirection::Column);
    panel->set_padding(12.0f, Unit::Dp);
    panel->set_gap(4.0f);
    panel->set_max_width(520.0f, Unit::Dp);
    panel->set_border_radius(theme::border::radius_lg, Unit::Dp);
    panel->set_background_color(theme::color::ModalOverlay);
    overlay.title = context.create_element<Label>(panel, "Online Lobby", LabelStyle::Normal);
    for (Label*& line : overlay.lines) {
        line = context.create_element<Label>(panel, "", LabelStyle::Small);
    }
    overlay.status = context.create_element<Label>(panel, "", LabelStyle::Small);
    overlay.status->set_color(theme::color::Warning);
    context.close();
    overlay.context = context;
    if (previous != ContextId::null()) {
        previous.open();
    }
}

void rush2::online::game::lobby_frame(uint8_t* rdram) {
    rush2::online_tab::follow_host_settings();
    State st = state();
    int gs = state_of(rdram);
    if (st == State::Offline || st == State::Connecting) {
        set_ports(Ports::Off);
        car_chosen = false;
        start_sent = false;
        after_race = false;
        stop_following_rules();
        rush2::track2049::set_all_tracks_open(false);
        set_profile_name({});
        update_overlay(false);
        return;
    }
    if (st != State::Lobby) {
        // The race is starting or running (online_game.cpp): the overlay is up only while it waits for someone
        // (race_notice).
        set_ports(gs == 2 ? Ports::Blocked : Ports::Off);
        update_overlay(!race_waiting.empty());
        return;
    }
    // A joiner's track select has every track, so the host's is on it.
    rush2::track2049::set_all_tracks_open(!is_host());
    frames_in_state = gs == last_state ? frames_in_state + 1 : 0;
    last_state = gs;
    pulse++;
    // Past Select Player the lobby shows the profile's name.
    if (gs == 1 || gs == 2) {
        set_profile_name(rush2::collectibles::player_name(rdram, 0));
    }
    if (gs != 0 && gs != 1 && gs != 2 && !is_host() && !after_race && MEM_BU(0, (int32_t)front_end_exit) != 0) {
        // A joiner's attract demo (the byte is set while it runs): START ends it.
        set_ports(Ports::Fed, pulsed(button_start));
        set_progress(Progress::Lobby);
        update_overlay(false);
        return;
    }
    if (gs != 0 && gs != 1 && gs != 2) {
        // After a race: the post-race (records, name entry) is this machine's own.
        set_ports(Ports::Own);
        set_progress(Progress::Lobby);
        update_overlay(false);
        return;
    }
    if (after_race) {
        // After the post-race the front end comes up: everyone goes back to the track select with the player they
        // picked, as Select Player sends them there.
        if (gs == 0) {
            if (screen_of(rdram) >= 0) {
                MEM_B(0, (int32_t)front_end_exit) = 2;
            }
            set_ports(Ports::Blocked);
            notice = "Back to the track select...";
            update_overlay(true);
            return;
        }
        after_race = false;
    }
    if (is_host()) {
        host_frame(rdram);
    }
    else {
        joiner_frame(rdram);
    }
    // Test aid: progress changes in the log.
    static std::string logged;
    int me = local_slot();
    std::vector<RosterEntry> r = roster();
    std::string now = "state " + std::to_string(gs) + " screen " + std::to_string(screen_of(rdram)) + " track " +
        std::to_string(MEM_BU(0, (int32_t)track_id)) + " progress " +
        (me >= 0 && me < (int)r.size() ? std::to_string(r[me].progress) : "?") + " ports " +
        std::to_string(ports_mode.load()) + ": " + notice;
    if (now != logged) {
        logged = now;
        std::fprintf(stderr, "[lobby] %s\n", now.c_str());
    }
    update_overlay(true);
}

void rush2::online::game::race_notice(const std::string& text) {
    if (text == race_waiting) {
        return;
    }
    race_waiting = text;
    notice = text;
    update_overlay(!text.empty());
}

void rush2::online::game::race_over() {
    after_race = true;
    car_chosen = false;
    start_sent = false;
}

bool rush2::online::game::controller_override(int port, uint16_t* buttons, float* x, float* y) {
    Ports mode = (Ports)ports_mode.load();
    if (mode == Ports::Off) {
        return false;
    }
    if (port != 0 || mode == Ports::Blocked) {
        *buttons = 0;
        *x = 0.0f;
        *y = 0.0f;
        return true;
    }
    switch (mode) {
        case Ports::Fed:
            *buttons = fed_buttons.load();
            *x = 0.0f;
            *y = 0.0f;
            return true;
        case Ports::NoConfirm:
            *buttons &= (uint16_t)~(button_a | button_start);
            return true;
        case Ports::BackOnly:
            *buttons &= button_b;
            *x = 0.0f;
            *y = 0.0f;
            return true;
        default:
            return false;
    }
}

bool rush2::online::game::car_select_done(uint8_t* rdram, recomp_context* ctx, bool* stay) {
    if (state() == State::Offline || state() == State::Connecting) {
        return false;
    }
    if (rush2::online::game::start_from_car_select(rdram, ctx)) {
        *stay = false;
        return true;
    }
    if (!car_chosen) {
        car_chosen = true;
        const char* forced = std::getenv("RUSH2_ONLINE_CAR");
        if (forced != nullptr && *forced != 0) {  // Test aid: this car type, whatever was picked.
            MEM_B(1, (int32_t)car_table) = (int8_t)std::atoi(forced);
        }
        CarChoice car;
        car.type = (uint8_t)MEM_BU(1, (int32_t)car_table);
        for (int i = 0; i < 4; i++) {
            car.colors[i] = (uint8_t)MEM_BU(2 + i, (int32_t)car_table);
        }
        // The profile the player races with, as the car select left it (online_game.cpp).
        std::vector<uint8_t> profile;
        uint32_t address = (uint32_t)MEM_W(record_profile, (int32_t)player_records);
        if (address >= 0x80000000u && address + profile_size <= 0x80800000u) {
            profile.resize(profile_size);
            for (uint32_t i = 0; i < profile_size; i++) {
                profile[i] = (uint8_t)MEM_BU(0, (int32_t)(address + i));
            }
        }
        set_car(car, profile);
    }
    *stay = true;
    return true;
}

bool rush2::online::game::car_select_back(uint8_t* rdram) {
    if (state() != State::Lobby || is_host()) {
        return false;
    }
    return !allow_back;
}
