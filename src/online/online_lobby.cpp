// Online lobbies in the game's own menus (docs/online.md#the-lobby): the host goes through Start Game, Select Player,
// the track select and the car select as in a local race, and every other player's game follows it.
//
// - Lobby: while the host is anywhere else in its menus, the others wait on the title screen (its attract demo held
//   off) with their controllers off.
// - Select Player: when the host opens it, everyone's game opens it too (the front end's next screen, 0x800E7BB4 = 1)
//   and each player picks their own profile. The host can't leave it (the front end loop holds it on the screen,
//   rush2_online_hold_front_end) until everyone has and is on the track select.
// - Track select: the host's. The others' track selects are turned to the host's track, then follow the host's
//   controller, read-only (their own controllers are off), so they go on to the car select with it.
// - Car select: everyone picks their own car. A player who has stays on the screen, controller off
//   (car_select_done), until all have; then the host starts the race and every car select starts it.
// The game's controller reads go through controller_override (main.cpp): ports 2-4 are off throughout, so nobody
// else joins on the same machine, and port 1 is the player's, off, or the host's controller.
//
// The race the host chose (race type, track, the track select's settings) goes to every machine with its progress
// (publish_menu) and sets the race up (online_game.cpp); the menus are only how the players get there.

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
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

namespace {
    using namespace rush2::online;

    // Game variables.
    constexpr uint32_t game_state = 0x8010C0D0;      // 0 front end, 1 track select, 2 car select, 7+ the race
    constexpr uint32_t front_end_screen = 0x800E7BC4;      // s16, -1 none (func_800AE0D4's screens)
    constexpr uint32_t front_end_next_screen = 0x800E7BB4; // s32: func_800AE288 switches to it
    constexpr int screen_select_player = 1;
    constexpr int menu_screens_end = 0xE;            // 0-0xD the menus; 0xE-0x12 the logos, title and attract demo
    constexpr int screen_title = 0x10;
    constexpr uint32_t attract_start = 0x8010C034;   // f32: when the title screen came up (its attract demo timer)
    constexpr uint32_t game_time = 0x80117488;       // f32
    constexpr uint32_t race_type = 0x8010C3E8;       // s32: 0 a race, 1 circuit, 2 practice (Start Game)
    constexpr uint32_t track_id = 0x8010C3F0;        // u8: the track select id
    constexpr uint32_t menu_settings = 0x800D5760;   // the track select's settings (laps, drones, options)
    constexpr uint32_t car_table = 0x800D9C10;       // 9 bytes per car slot: +1 type, +2-+5 colors and stripe

    constexpr uint16_t button_a = 0x8000, button_b = 0x4000, dpad_right = 0x0100;
    constexpr int settle_frames = 20;  // On the track select this long before it takes input.

    // How the game's controller ports read (controller_override, called from the threads that read them).
    enum class Ports : int { Off, Own, Blocked, Fed };
    std::atomic<int> ports_mode = (int)Ports::Off;
    std::atomic<uint32_t> fed_input = 0;  // buttons | stick_x << 16 | stick_y << 24

    void set_ports(Ports mode, uint16_t buttons = 0, int8_t x = 0, int8_t y = 0) {
        fed_input = (uint32_t)buttons | ((uint32_t)(uint8_t)x << 16) | ((uint32_t)(uint8_t)y << 24);
        ports_mode = (int)mode;
    }

    // This machine's place in the lobby.
    bool car_chosen = false;       // Picked a car on the car select (car_select_done).
    bool start_sent = false;       // Host: asked for the race to start.
    bool hold_front_end = false;   // Host: keep the front end on Select Player (rush2_online_hold_front_end).
    bool allow_back = false;       // Backing out of the car select is how this machine follows the host back.
    int frames_in_state = 0;
    int last_state = -1;
    int pulse = 0;
    int idle_feed = 0;
    std::string notice;            // What the lobby waits for, for the overlay.

    int32_t state_of(uint8_t* rdram) { return MEM_W(0, (int32_t)game_state); }
    int screen_of(uint8_t* rdram) { return MEM_H(0, (int32_t)front_end_screen); }

    RaceSettings menu_settings_now(uint8_t* rdram) {
        RaceSettings s;
        s.race_type = (uint8_t)MEM_W(0, (int32_t)race_type);
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
        bool unsupported = others && (s.race_type == 1 || s.race_type == 2);

        if (phase != MenuPhase::Car) {
            car_chosen = false;
            start_sent = false;
        }
        set_progress(phase == MenuPhase::Car ? (car_chosen ? Progress::CarChosen : Progress::ChoosingCar) :
            phase == MenuPhase::Track ? Progress::TrackSelect :
            phase == MenuPhase::SelectPlayer ? Progress::ChoosingPlayer : Progress::Lobby);

        bool all_on_track = all_at_least(r, me, Progress::TrackSelect);
        hold_front_end = phase == MenuPhase::SelectPlayer && (unsupported || !all_on_track);
        bool input_open = phase == MenuPhase::Track && all_on_track && frames_in_state >= settle_frames;

        if (phase == MenuPhase::Track && !input_open) {
            set_ports(Ports::Blocked);
        }
        else if (phase == MenuPhase::Car && car_chosen) {
            set_ports(Ports::Blocked);
        }
        else {
            set_ports(Ports::Own);
        }

        MenuInput in;
        uint16_t buttons = 0;
        float x = 0.0f, y = 0.0f;
        if (rush2::online::game::read_local_controller(&buttons, &x, &y)) {
            in.buttons = buttons;
            in.stick_x = (int8_t)std::clamp(x * 127.0f, -127.0f, 127.0f);
            in.stick_y = (int8_t)std::clamp(y * 127.0f, -127.0f, 127.0f);
        }
        MenuState m;
        m.phase = phase;
        m.settings = s;
        m.input_open = input_open;
        publish_menu(m, in);

        if (unsupported && phase == MenuPhase::SelectPlayer) {
            notice = "Online races are One Race, Stunt and Battle: back out and pick one of those.";
        }
        else if (phase == MenuPhase::SelectPlayer && !all_on_track) {
            notice = "Waiting for " + list_names(r, me, Progress::TrackSelect) + " to choose a player.";
        }
        else if (phase == MenuPhase::Track && !input_open) {
            std::string names = list_names(r, me, Progress::TrackSelect);
            notice = names.empty() ? "Getting everyone to the track select..." : "Waiting for " + names + ".";
        }
        else if (phase == MenuPhase::Car && !all_at_least(r, me, Progress::CarChosen)) {
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
        std::string host = r.empty() ? "the host" : r[0].name;
        pulse++;
        allow_back = false;
        if (gs != 2) {
            car_chosen = false;
        }

        if (hm.phase == MenuPhase::Lobby) {
            if (gs == 1 || gs == 2) {
                // The host left the race menus: back out to the front end.
                allow_back = true;
                set_ports(Ports::Fed, pulse % 4 < 2 ? button_b : 0);
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
            // Everyone picks their own player first, with the host's Start Game choice.
            MEM_W(0, (int32_t)race_type) = hm.settings.race_type;
            if (screen == screen_select_player) {
                set_ports(Ports::Own);
                notice = "Choose a player.";
            }
            else {
                if (screen == MEM_W(0, (int32_t)front_end_next_screen) && screen >= 0) {
                    MEM_W(0, (int32_t)front_end_next_screen) = screen_select_player;
                }
                set_ports(Ports::Blocked);
                notice.clear();
            }
            set_progress(Progress::ChoosingPlayer);
            return;
        }

        if (gs == 1) {
            bool aligned = MEM_BU(0, (int32_t)track_id) == hm.settings.track;
            if (hm.phase == MenuPhase::Track && hm.input_open) {
                // The host's controller drives this track select.
                MenuInput in;
                if (next_menu_input(&in)) {
                    set_ports(Ports::Fed, in.buttons, in.stick_x, in.stick_y);
                }
                else {
                    set_ports(Ports::Fed);
                }
                notice = host + " is choosing the track.";
            }
            else if (hm.phase == MenuPhase::Car) {
                // The host went on to the car select: so does this one, with the host's input that took it there
                // (or an A press if that didn't come).
                MenuInput in;
                if (next_menu_input(&in)) {
                    set_ports(Ports::Fed, in.buttons, in.stick_x, in.stick_y);
                    idle_feed = 0;
                }
                else {
                    set_ports(Ports::Fed, ++idle_feed > 45 && pulse % 4 < 2 ? button_a : 0);
                }
                notice.clear();
            }
            else if (!aligned && frames_in_state >= settle_frames) {
                // Turn to the host's track before its controller takes over.
                set_ports(Ports::Fed, pulse % 6 < 2 ? dpad_right : 0);
                notice = "Waiting for " + host + ".";
            }
            else {
                set_ports(Ports::Blocked);
                notice = "Waiting for " + host + ".";
            }
            set_progress(aligned && frames_in_state >= settle_frames ? Progress::TrackSelect : Progress::ChoosingPlayer);
            return;
        }

        if (gs == 2) {
            idle_feed = 0;
            if (hm.phase != MenuPhase::Car) {
                // The host went back to the track select.
                allow_back = true;
                set_ports(Ports::Fed, pulse % 4 < 2 ? button_b : 0);
                set_progress(Progress::ChoosingCar);
                notice.clear();
                return;
            }
            set_ports(car_chosen ? Ports::Blocked : Ports::Own);
            set_progress(car_chosen ? Progress::CarChosen : Progress::ChoosingCar);
            notice = car_chosen ? "Waiting for " + list_names(r, local_slot(), Progress::CarChosen) + " to pick a car." :
                "Pick your car.";
            return;
        }
    }

    // The overlay: the lobby's players and what it waits for, over the game (it takes no input).
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
    Element* panel = context.create_element<Element>(context.get_root_element());
    panel->set_position(Position::Absolute);
    panel->set_top(16.0f, Unit::Dp);
    panel->set_left(16.0f, Unit::Dp);
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
    State st = state();
    if (st == State::Offline || st == State::Connecting) {
        set_ports(Ports::Off);
        hold_front_end = false;
        car_chosen = false;
        update_overlay(false);
        return;
    }
    if (st != State::Lobby) {
        // The race is starting: the car select keeps the controllers as they are until it goes.
        update_overlay(false);
        return;
    }
    int gs = state_of(rdram);
    frames_in_state = gs == last_state ? frames_in_state + 1 : 0;
    last_state = gs;
    if (gs != 0 && gs != 1 && gs != 2) {
        // After a race: the post-race (records, name entry) is this machine's own.
        set_ports(Ports::Own);
        hold_front_end = false;
        update_overlay(false);
        return;
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
    std::string now = "state " + std::to_string(gs) + " screen " + std::to_string(screen_of(rdram)) + " progress " +
        (me >= 0 && me < (int)r.size() ? std::to_string(r[me].progress) : "?") + " ports " +
        std::to_string(ports_mode.load()) + ": " + notice;
    if (now != logged) {
        logged = now;
        std::fprintf(stderr, "[lobby] %s\n", now.c_str());
    }
    update_overlay(true);
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
    if (mode == Ports::Fed) {
        uint32_t fed = fed_input.load();
        *buttons = (uint16_t)fed;
        *x = (int8_t)(fed >> 16) / 127.0f;
        *y = (int8_t)(fed >> 24) / 127.0f;
        return true;
    }
    return false;
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
        CarChoice car;
        car.type = (uint8_t)MEM_BU(1, (int32_t)car_table);
        for (int i = 0; i < 4; i++) {
            car.colors[i] = (uint8_t)MEM_BU(2 + i, (int32_t)car_table);
        }
        set_car(car);
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

// The front end loop (func_800AE288 at 0x800AE2AC, $t6 = 0x800FAE6C: 2 once Select Player is done, which leaves the
// front end for the track select): the host stays on Select Player until every player is on the track select.
extern "C" int rush2_online_hold_front_end(uint8_t* rdram, recomp_context* ctx) {
    return is_host() && state() == State::Lobby && hold_front_end ? 1 : 0;
}
