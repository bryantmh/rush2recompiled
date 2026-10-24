// Online lobbies in the game's own menus (docs/online.md#the-lobby): the host goes through Start Game, Select Player,
// the track select and the car select as in a local race, and every other player's game follows it.
//
// - Lobby: while the host is anywhere else in its menus, the others wait on the title screen (its attract demo held
//   off) with their controllers off.
// - Select Player: when the host chooses One Race, Circuit, Stunt or Battle on Start Game, everyone's game goes there
//   too (START on the title screen, the same choice on Start Game) and each player picks their own profile. The host
//   is never held: it goes on to its track select, which only takes its confirm once everyone is on theirs.
// - Circuit menu (a circuit): the host's choice (CONTINUE, NEW CIRCUIT). The others wait on theirs and, once the host
//   is on its track select, take the host's circuit (online_game.cpp, follow_circuit) and go on as CONTINUE does.
// - Track select: the host's. The others' track selects show the host's every frame (track, carousel, options and
//   cursor; their controllers are off) and confirm when the host's does, so they go on to the car select with it.
// - Car select: everyone picks their own car. A player who has stays on the screen (car_select_done) until all
//   have; then the host starts the race and every car select starts it. A player who picked can always change their
//   car: B takes them to the track select, which goes on to the car select again as the host's is there. Why the host's
//   race can't start (MenuState::start_error) shows on every machine, so the player it names knows what to do.
// Each machine's progress comes from where its own game is, never from what it was told, so a player who backs out
// or joins late is waited for and nobody waits on a step that already happened.
// The game's controller reads go through controller_override (main.cpp): where the machine's players choose (Select
// Player, where players 2-4 join with START as in a local race, and the car select) every port is theirs; elsewhere
// they are masked, off, or port 1 is pressed for the player. A machine with several players gets a slot per player
// once its car select has them all (car_select_done, rush2::online::set_cars), and they race in split screen.
//
// The race the host chose (race type, track, the track select's settings) goes to every machine with its progress
// (publish_menu) and sets the race up (online_game.cpp); the menus are only how the players get there.

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
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
#include "calspeed.h"
#include "collectibles.h"
#include "track2049.h"
#include "car2049.h"
#include "players4.h"
#include "circuit.h"
#include "car_mods.h"
#include "car_types.h"
#include "wings.h"

namespace {
    using namespace rush2::online;
    using rush2::track2049::StartKind;

    // Game variables.
    constexpr uint32_t game_state = 0x8010C0D0;      // 0 front end, 1 track select, 2 car select, 7+ the race
    constexpr uint32_t front_end_screen = 0x800E7BC4;      // s16, -1 none (func_800AE0D4's screens)
    constexpr uint32_t front_end_next_screen = 0x800E7BB4; // s32: func_800AE288 switches to it
    constexpr int screen_start_game = 0;
    constexpr int screen_select_player = 1;
    constexpr int screen_circuit = 0xA;              // The circuit menu (CONTINUE, NEW CIRCUIT; func_803B7018)
    constexpr int screen_circuit_view = 0xB;         // Its standings (func_803B6260)
    constexpr int screen_victory = 0xC;              // A finished circuit's victory screen (func_800ABE7C)
    constexpr int menu_screens_end = 0xE;            // 0-0xD the menus; 0xE-0x12 the logos, title and attract demo
    constexpr int screen_title = 0x10;
    constexpr uint32_t attract_start = 0x8010C034;   // f32: when the title screen came up (its attract demo timer)
    constexpr uint32_t game_time = 0x80117488;       // f32
    // u8: in the front end, 2 leaves it for the track select (Select Player); nonzero while the attract demo runs.
    constexpr uint32_t front_end_exit = 0x800FAE6C;
    constexpr uint32_t start_game_row = 0x80023064;  // u16: the Start Game menu's row (func_803B12C8)
    constexpr uint32_t race_type = 0x8010C3E8;       // s32: 0 a race, 1 circuit, 2 practice (Start Game)
    constexpr uint32_t track_id = 0x8010C3F0;        // u8: the track select id
    constexpr uint32_t circuit_block_ptr = 0x800D3E40; // The circuit block (src/online/online_game.cpp)
    constexpr uint32_t circuit_races = 0x800D3A60;   // A circuit's races, 4 bytes each: +0 the track
    constexpr uint32_t circuit_race_index = 0x800D3DF0;  // u8: the circuit's current race
    constexpr uint32_t menu_settings = 0x800D5760;   // the track select's settings (laps, drones, options)
    constexpr uint32_t car_table = 0x800D9C10;       // 9 bytes per car slot: +1 type, +2-+5 colors and stripe
    constexpr int32_t car_entry_size = 9;
    constexpr uint32_t player_records = 0x800C2140;  // 0x28 each: +0x24 the save profile
    constexpr int32_t player_record_size = 0x28;
    constexpr int32_t record_automatic = 0x20;       // u8: the car select's TRANSMISSION, 1 automatic
    constexpr int32_t record_profile = 0x24;
    constexpr uint32_t profile_size = 0x6C0;
    // The track select (func_803ABE0C; src/rush2049/track2049_menu.cpp): the carousel entry shown (s16), the option
    // row under the cursor (s32), the top option row on screen (s32) and the option list's length (s32).
    constexpr uint32_t carousel_shown = 0x803D05B4;
    constexpr uint32_t carousel_count = 0x803D05AE;  // s16: entries in the carousel
    constexpr uint32_t option_cursor = 0x803D05BC;
    constexpr uint32_t option_top = 0x803D05C4;
    constexpr uint32_t option_count = 0x803D05CC;

    constexpr uint16_t button_a = 0x8000, button_b = 0x4000, button_start = 0x1000;
    constexpr int settle_frames = 20;  // On the track select this long before it takes a confirm.

    // How the game's controller ports read (controller_override, called from the threads that read them). The machine
    // may have up to four players (src/players4.cpp), one per port.
    enum class Ports : int {
        Off,        // Not online: the controllers as they are.
        Own,        // The players' controllers.
        NoConfirm,  // The players', without A and START.
        BackOnly,   // The players' B only.
        Blocked,    // Nothing.
        Fed,        // Buttons pressed for player 1 (fed_buttons) on port 1, nothing on the others.
    };
    std::atomic<int> ports_mode = (int)Ports::Off;
    std::atomic<uint16_t> fed_buttons = 0;

    void set_ports(Ports mode, uint16_t buttons = 0) {
        fed_buttons = buttons;
        ports_mode = (int)mode;
    }

    // Host: the tracks its track select offered when last up (rush2::track2049::offered_tracks), sent with the menu
    // state; cleared away from the selects, where the next select may be of another kind.
    std::vector<uint8_t> host_offered;

    // This machine's place in the lobby.
    bool car_chosen = false;       // Picked a car on the car select (car_select_done).
    bool start_sent = false;       // Host: asked for the race to start.
    bool allow_back = false;       // Backing out of the car select is how this machine follows the host back.
    bool after_race = false;       // Back to the track select once the post-race is done.
    // Host: why its race couldn't start (start_race's error), published for every machine (MenuState::start_error).
    std::string start_problem;
    int frames_in_state = 0;
    int last_state = -1;
    int pulse = 0;
    std::string notice;            // What the lobby waits for, for the overlay.
    std::string race_waiting;      // In a race: who it waits for (race_notice).
    // A joiner who lost its lobby (the host closed it or quit its game, or the connection dropped): the reason
    // (rush2::online::last_error), shown in the overlay for dropped_time once the session is gone.
    std::string dropped;
    std::chrono::steady_clock::time_point dropped_at;
    constexpr auto dropped_time = std::chrono::seconds(8);

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
        return kind == StartKind::OneRace || kind == StartKind::Circuit || kind == StartKind::Stunt ||
            kind == StartKind::Battle;
    }

    const char* kind_name(StartKind kind) {
        switch (kind) {
            case StartKind::Circuit: return "Circuit";
            case StartKind::Stunt: return "Stunt";
            case StartKind::Battle: return "Battle";
            default: return "One Race";
        }
    }

    // On Start Game, its row; past it, what it chose. Leaving Start Game pushes its row onto the front end's row stack
    // and zeroes it (func_803B12C8), so the row read One Race on Select Player and the track select: the host
    // published One Race for a Stunt or Battle, the joiners chose it, and their race selects showed the race tracks'
    // dioramas under the host's arena. A circuit is known by the race type too (the circuit menu keeps its cursor in the
    // Start Game row).
    StartKind own_start_kind(uint8_t* rdram) {
        bool on_start_game = state_of(rdram) == 0 && screen_of(rdram) == screen_start_game;
        if (MEM_W(0, (int32_t)race_type) == 1 && !on_start_game) {
            return StartKind::Circuit;
        }
        StartKind chosen = rush2::track2049::chosen_start_kind();
        if (!on_start_game && chosen != StartKind::None) {
            return chosen;
        }
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
        // tracks, 18-24 SF Rush's, 25-28 Rush 2049's stunt arenas, 29 its obstacle course, 30-37 its battle arenas,
        // 64-79 California Speed's tracks.
        int t = s.track;
        s.mode = t == 11 || (t >= 25 && t <= 28) ? GameMode::Stunt : t >= 30 && t <= 37 ? GameMode::Battle : GameMode::Race;
        s.needs_games = (t >= 12 && t <= 17) || (t >= 25 && t <= 37) ? games_rush2049 : (t >= 18 && t <= 24) ? games_rush1
                      : rush2::calspeed::is_menu_track(t) ? games_calspeed : 0;
        s.battle_points = (uint8_t)rush2::battle::points_to_win();
        s.battle_minutes = (uint8_t)rush2::battle::time_limit_minutes();
        // The BATTLE row arms the race only with the Rush 2049 tracks on (rush2::battle::race_battle_applies): then
        // every machine needs Rush 2049's ROM for it, or the race would have weapons on some machines only.
        s.race_battle = rush2::battle::race_battle() && rush2::track2049::available() ? 1 : 0;
        if (s.race_battle != 0) {
            s.needs_games |= games_rush2049;
        }
        s.mod_track = rush2::track2049::online_track_key(t);
        s.mod_track_name = rush2::track2049::mod_track_name(t);
        s.circuit = rush2::online::game::read_circuit(rdram);
        return s;
    }

    // The track select id of the host's track on this machine: a custom track by its key (each machine numbers its
    // own), -1 if this machine hasn't it; the other tracks by their id.
    int local_track(const RaceSettings& s) {
        return s.mod_track.empty() ? s.track : rush2::track2049::track_for_online_key(s.mod_track);
    }

    // Whether slot i is another connected machine than `me` (the machine's first slot): the lobby waits on machines,
    // whose players progress together, so each is named once, by its first player.
    bool other_machine(const std::vector<RosterEntry>& r, int i, int me) {
        return r[i].used && r[i].connected && r[i].machine != me && r[i].local_index == 0;
    }

    // The other connected players who lack the host's custom track, and those who haven't said yet whether they
    // have it (they checked for another).
    std::string lacking_track(const std::vector<RosterEntry>& r, int me, const RaceSettings& s, bool* unchecked) {
        std::string names;
        uint32_t check = track_check(s.mod_track);
        *unchecked = false;
        if (check == 0) {
            return names;
        }
        for (int i = 0; i < (int)r.size(); i++) {
            if (!other_machine(r, i, me)) {
                continue;
            }
            if (r[i].track_checked != check) {
                *unchecked = true;
            }
            else if (r[i].lacks_track) {
                names += (names.empty() ? "" : ", ") + r[i].name;
            }
        }
        return names;
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
            if (other_machine(r, i, me) && r[i].progress < uint8_t(below)) {
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
            if (other_machine(r, i, me) && (r[i].games & games) != games) {
                names += (names.empty() ? "" : ", ") + r[i].name;
            }
        }
        return names;
    }

    // Whether a circuit's last race has been run (the circuit block's races finished, +2, reached its length).
    bool circuit_finished(uint8_t* rdram) {
        uint32_t block = (uint32_t)MEM_W(0, (int32_t)circuit_block_ptr);
        return MEM_W(0, (int32_t)race_type) == 1 && block >= 0x80000000u && block < 0x80800000u &&
            (int8_t)MEM_B(2, (int32_t)block) >= rush2::circuit::length(rdram);
    }

    // The ROMs in a games_* mask by name ("SF Rush and Rush 2049").
    std::string games_names(uint8_t games) {
        std::vector<std::string> names;
        if (games & games_rush1) names.push_back("SF Rush");
        if (games & games_rush2049) names.push_back("Rush 2049");
        if (games & games_calspeed) names.push_back("California Speed");
        std::string out;
        for (size_t i = 0; i < names.size(); i++) {
            out += (i == 0 ? "" : i + 1 == names.size() ? " and " : ", ") + names[i];
        }
        return out;
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
    // A custom track this machine hasn't leaves the track as it is (the host can't start it; host_frame).
    void follow_track_select(uint8_t* rdram, const MenuState& hm) {
        const RaceSettings& s = hm.settings;
        int t = local_track(s);
        if (t >= 0) {
            MEM_B(0, (int32_t)track_id) = (int8_t)t;
            // The carousel moves one entry per change of the shown entry: each frame the track select
            // (trackselect_init_draw_803AB294, 0x803ABB1C-0x803ABCC4) shifts an entry's target by one place when its
            // place relative to the shown one (+0x14) changed, whatever the change. A jump of several entries left
            // the dioramas out of line with the track, so the shown entry steps toward the host's one entry a
            // frame, the shorter way round, as scrolling does.
            int index = rush2::track2049::carousel_index(rdram, t);
            int count = MEM_H(0, (int32_t)carousel_count);
            int shown = MEM_H(0, (int32_t)carousel_shown);
            if (index >= 0 && count > 0 && shown != index) {
                int ahead = ((index - shown) % count + count) % count;
                int step = ahead <= count - ahead ? 1 : -1;
                MEM_H(0, (int32_t)carousel_shown) = (int16_t)(((shown + step) % count + count) % count);
            }
        }
        for (int i = 0; i < (int)s.menu.size(); i++) {
            MEM_B(i, (int32_t)menu_settings) = (int8_t)s.menu[i];
        }
        // A circuit's track select shows the circuit's race (0x800D3A60[0x800D3DF0]): the host's.
        if (!s.circuit.empty()) {
            rush2::online::game::follow_circuit(rdram, s.circuit);
            int index = MEM_BU(0, (int32_t)circuit_race_index);
            if (t >= 0 && index < 28) {
                MEM_B(0, (int32_t)(circuit_races + index * 4)) = (int8_t)t;
            }
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
        int screen = screen_of(rdram);
        MenuPhase phase = gs == 1 ? MenuPhase::Track : gs == 2 ? MenuPhase::Car :
            gs != 0 ? MenuPhase::Lobby :
            screen == screen_select_player ? MenuPhase::SelectPlayer :
            screen == screen_circuit || screen == screen_circuit_view ? MenuPhase::Circuit : MenuPhase::Lobby;
        std::vector<RosterEntry> r = roster();
        int me = local_slot();
        RaceSettings s = menu_settings_now(rdram);
        bool others = false;
        for (int i = 0; i < (int)r.size(); i++) {
            others |= other_machine(r, i, me);
        }
        // Practice, Ghost Race and the profile screens (Records, Unlocks) aren't raced online: the others stay in the
        // lobby.
        bool unsupported = phase != MenuPhase::Lobby && !supported(StartKind(s.start_kind));

        if (phase != MenuPhase::Car) {
            car_chosen = false;
            start_sent = false;
            start_problem.clear();
        }
        set_progress(phase == MenuPhase::Car ? (car_chosen ? Progress::CarChosen : Progress::ChoosingCar) :
            phase == MenuPhase::Track ? Progress::TrackSelect :
            phase == MenuPhase::SelectPlayer || phase == MenuPhase::Circuit ? Progress::ChoosingPlayer : Progress::Lobby);

        bool all_on_track = all_at_least(r, me, Progress::TrackSelect);
        std::string lacking = missing_games(r, me, s.needs_games);
        bool unchecked = false;
        std::string lacking_mod = lacking_track(r, me, s, &unchecked);
        // A custom track goes by its key (the machines number their custom tracks in their own mod order): one whose
        // files haven't been read and hashed yet has none, and the others would race whatever their own id names.
        bool keyless_mod = s.track >= rush2::track2049::mod_menu_id && s.track < rush2::calspeed::first_menu_id &&
            s.mod_track.empty();
        bool can_confirm = !unsupported && all_on_track && lacking.empty() && lacking_mod.empty() && !unchecked &&
            !keyless_mod && frames_in_state >= settle_frames;

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

        if (gs == 1) {
            host_offered.clear();
            for (int t : rush2::track2049::offered_tracks(rdram)) {
                host_offered.push_back((uint8_t)t);
            }
        }
        else if (gs != 2) {
            host_offered.clear();
        }

        MenuState m;
        m.phase = unsupported ? MenuPhase::Lobby : phase;
        m.settings = s;
        m.option_cursor = (int8_t)MEM_W(0, (int32_t)option_cursor);
        m.option_top = (int8_t)MEM_W(0, (int32_t)option_top);
        m.start_error = start_problem;
        m.offered_tracks = host_offered;
        publish_menu(m);

        // A Battle needs the Rush 2049 ROM for its Start Game row (start_menu_row): a player without it can't follow.
        std::string no_battle = StartKind(s.start_kind) == StartKind::Battle ? missing_games(r, me, games_rush2049) : "";

        if (unsupported && others) {
            notice = "Online races are One Race, Circuit, Stunt and Battle: back out to Start Game and pick one of those.";
        }
        else if (phase != MenuPhase::Lobby && !no_battle.empty()) {
            notice = no_battle + (no_battle.find(',') == std::string::npos ? " doesn't" : " don't") +
                " have the Rush 2049 ROM a Battle needs: back out to Start Game and pick another race.";
        }
        // A player who joined while the host was on the car select may lack the track: they stay on their track
        // select and the race doesn't start (below) until the host goes back and picks another.
        else if ((phase == MenuPhase::Track || phase == MenuPhase::Car) && !lacking.empty()) {
            bool several = (s.needs_games & (s.needs_games - 1)) != 0;
            notice = lacking + (lacking.find(',') == std::string::npos ? " doesn't" : " don't") + " have the " +
                games_names(s.needs_games) + (several ? " ROMs" : " ROM") + " this race needs" +
                (s.race_battle != 0 ? " (the BATTLE row's weapons are Rush 2049's)." : ".");
        }
        else if (phase == MenuPhase::Track && keyless_mod && others) {
            notice = "Reading the custom track's files...";
        }
        else if ((phase == MenuPhase::Track || phase == MenuPhase::Car) && !lacking_mod.empty()) {
            notice = lacking_mod + (lacking_mod.find(',') == std::string::npos ? " doesn't" : " don't") +
                " have the custom track " + s.mod_track_name + " installed (or has another version of it).";
        }
        else if (phase == MenuPhase::Track && !all_on_track) {
            notice = "Waiting for " + list_names(r, me, Progress::TrackSelect) + " to get to the track select.";
        }
        else if (phase == MenuPhase::Car && car_chosen && !all_at_least(r, me, Progress::CarChosen)) {
            notice = "Waiting for " + list_names(r, me, Progress::CarChosen) + " to pick a car.";
        }
        else if (phase == MenuPhase::Lobby) {
            notice = others ? "Start Game > One Race, Circuit, Stunt or Battle takes everyone to Select Player." :
                "Waiting for players to join.";
        }
        else {
            notice.clear();
        }

        if (phase == MenuPhase::Car && car_chosen && !start_sent && all_at_least(r, me, Progress::CarChosen) &&
            lacking.empty() && lacking_mod.empty() && !unchecked) {
            start_sent = start_race(s);
            start_problem = start_sent ? std::string() : last_error();
            if (!start_sent) {
                notice = start_problem;
            }
        }
        else if (!(phase == MenuPhase::Car && car_chosen)) {
            start_problem.clear();
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
        if (kind != StartKind::Circuit && hm.phase != MenuPhase::Lobby) {
            rush2::online::game::follow_circuit(rdram, {});
        }
        // The host's unlocks and game filter: this machine's track select offers the tracks the host's does.
        rush2::track2049::set_online_offered(rdram, std::vector<int>(hm.offered_tracks.begin(), hm.offered_tracks.end()));

        if (hm.phase == MenuPhase::Lobby) {
            if (gs == 1 || gs == 2) {
                // The host left the race menus: back out to the front end.
                allow_back = true;
                set_ports(Ports::Fed, pulsed(button_b, 4));
            }
            else if (screen == screen_victory) {
                // This machine's victory screen after the host's circuit: the player's own, until START.
                set_ports(Ports::Own);
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
            bool race_on = std::any_of(r.begin(), r.end(), [](const RosterEntry& e) { return e.used && e.in_race; });
            notice = race_on ? "A race is under way: you'll race in the next one." :
                "Waiting for " + host + " to start a race.";
            return;
        }

        if (gs == 0) {
            // Everyone picks their own player first, getting there as the host did: START on the title screen (which
            // makes this machine's controller player 1, func_800A6B58), the host's choice on Start Game (its row
            // differs by machine), then Select Player.
            int row = rush2::track2049::start_menu_row(kind);
            if (screen == screen_select_player && own_start_kind(rdram) != kind) {
                // Start Game chose another kind of race than the host's (the host went back and chose another, or
                // this machine took its choice before the host's was known): back to Start Game for the host's.
                set_ports(Ports::Fed, pulsed(button_b));
                notice.clear();
            }
            else if (screen == screen_select_player) {
                set_ports(Ports::Own);
                notice = "Choose a player.";
            }
            else if (screen == screen_circuit || screen == screen_circuit_view) {
                // A circuit's menu (Select Player went on to it). The circuit is the host's: once the host has gone on
                // to its track select, this machine takes the host's circuit and follows, as CONTINUE would.
                if (kind == StartKind::Circuit && (hm.phase == MenuPhase::Track || hm.phase == MenuPhase::Car) &&
                    !hm.settings.circuit.empty()) {
                    rush2::online::game::follow_circuit(rdram, hm.settings.circuit);
                    if (screen == screen_circuit) {
                        MEM_B(0, (int32_t)front_end_exit) = 2;
                        set_ports(Ports::Blocked);
                    }
                    else {
                        set_ports(Ports::Fed, pulsed(button_b));
                    }
                    notice.clear();
                }
                else {
                    set_ports(Ports::Blocked);
                    notice = host + " is choosing the circuit.";
                }
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
            bool lacks = local_track(hm.settings) < 0;
            set_track_check(track_check(hm.settings.mod_track), lacks);
            if (lacks) {
                // The host can't start a custom track this machine hasn't (host_frame).
                set_ports(Ports::Blocked);
                notice = "You don't have the custom track " + hm.settings.mod_track_name + " installed (or have " +
                    "another version of it): " + host + " can't start it.";
            }
            else if (hm.phase == MenuPhase::Car && frames_in_state >= settle_frames) {
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
            // A player who picked keeps B, which takes them back to change their car (car_select_back).
            set_ports(car_chosen ? Ports::BackOnly : Ports::Own);
            set_progress(car_chosen ? Progress::CarChosen : Progress::ChoosingCar);
            notice = !car_chosen ? "Pick your car." : !hm.start_error.empty() ? hm.start_error :
                all_at_least(r, local_slot(), Progress::CarChosen) ? "Waiting for " + host + " to start the race." :
                "Waiting for " + list_names(r, local_slot(), Progress::CarChosen) + " to pick a car. (B changes your car.)";
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
        // Offline only to show why the lobby was lost: no lobby to name.
        std::string title = state() == State::Offline ? std::string("Online") : "Online Lobby: " + lobby_name();
        std::string text = title + "|" + notice;
        std::string lines[max_players];
        for (int i = 0; i < max_players && i < (int)r.size(); i++) {
            if (r[i].used) {
                lines[i] = r[i].name + (r[i].machine == me ? " (you)" : "") + "  -  " +
                    (r[i].connected ? progress_text(r[i].progress) : "left") +
                    (r[i].machine != me && r[i].connected && r[i].machine != 0 && is_host() ? "  " + std::to_string(r[i].ping_ms) + " ms" : "");
            }
            text += "|" + lines[i];
        }
        if (text != overlay.shown_text) {
            overlay.shown_text = text;
            overlay.context.open();
            overlay.title->set_text(title);
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
    static_assert(rush2::car_types::count_mod == max_mod_cars);
    set_local_mod_cars(rush2::online::game::mod_car_checks());
    State st = state();
    int gs = state_of(rdram);
    if (take_lobby_lost()) {
        dropped = last_error();
        dropped_at = std::chrono::steady_clock::now();
        rush2::online::session_log("[lobby] lost the lobby: %s\n", dropped.c_str());
    }
    if (st != State::Offline) {
        dropped.clear();
    }
    if (st == State::Offline || st == State::Connecting) {
        set_ports(Ports::Off);
        car_chosen = false;
        start_sent = false;
        after_race = false;
        stop_following_rules();
        rush2::online::game::follow_circuit(rdram, {});
        rush2::track2049::set_all_tracks_open(false);
        rush2::track2049::set_online_offered(rdram, {});
        host_offered.clear();
        set_profile_name({});
        race_waiting.clear();
        bool show_dropped = !dropped.empty() && std::chrono::steady_clock::now() - dropped_at < dropped_time;
        notice = show_dropped ? dropped : std::string();
        update_overlay(show_dropped);
        return;
    }
    if (st != State::Lobby) {
        // The race is starting or running (online_game.cpp): the overlay is up only while it waits for someone
        // (race_notice). A joiner who had just backed out of the car select (to change car) as the host started is on
        // the track select: A takes it back in, where the race starts.
        pulse++;
        if (st == State::Loading && gs == 1 && !is_host()) {
            set_ports(Ports::Fed, pulsed(button_a, 4));
        }
        else {
            set_ports(gs == 2 ? Ports::Blocked : Ports::Off);
        }
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
        // picked, as Select Player sends them there (for a circuit, to its next race, as CONTINUE does). A finished
        // circuit goes on to its victory screen instead (the circuit menu sends it there).
        if (gs == 0 && circuit_finished(rdram)) {
            after_race = false;
        }
        else if (gs == 0) {
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
        rush2::online::session_log("[lobby] %s\n", now.c_str());
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
    if (mode == Ports::Off || mode == Ports::Own) {
        // The machine's every controller: players 2-4 join on Select Player and pick their cars (src/players4.cpp).
        return false;
    }
    if (mode == Ports::Blocked || (mode == Ports::Fed && port != 0)) {
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
        // This machine's players, 1-4 (src/players4.cpp: player i on record i, car table entry i).
        int count = std::clamp(rush2::players4::joined_players(rdram), 1, (int)max_local_players);
        std::vector<LocalPlayer> players(count);
        for (int p = 0; p < count; p++) {
            LocalPlayer& lp = players[p];
            int32_t entry = (int32_t)(car_table + p * car_entry_size);
            int32_t record = (int32_t)(player_records + p * player_record_size);
            const char* forced = std::getenv("RUSH2_ONLINE_CAR");
            if (forced != nullptr && *forced != 0) {  // Test aid: this car type, whatever was picked.
                MEM_B(1, entry) = (int8_t)std::atoi(forced);
            }
            lp.car.type = (uint8_t)MEM_BU(1, entry);
            for (int i = 0; i < 4; i++) {
                lp.car.colors[i] = (uint8_t)MEM_BU(2 + i, entry);
            }
            lp.car.automatic = (uint8_t)MEM_BU(record_automatic, record);
            lp.car.wing_style = (uint8_t)rush2::wings::saved_style(p);
            lp.name = rush2::collectibles::player_name(rdram, p);
            // The profile the player races with, as the car select left it, and its Rush 2049 car options
            // (online_game.cpp).
            uint32_t address = (uint32_t)MEM_W(record_profile, record);
            if (address >= 0x80000000u && address + profile_size <= 0x80800000u) {
                lp.profile.resize(profile_size);
                for (uint32_t i = 0; i < profile_size; i++) {
                    lp.profile[i] = (uint8_t)MEM_BU(0, (int32_t)(address + i));
                }
                lp.side = rush2::car2049::side_block(rdram, address);
            }
            // A mod car's physics, for a machine that hasn't it (docs/online.md#mod-cars).
            if (rush2::car_types::is_mod(lp.car.type)) {
                lp.mod_spec = rush2::car2049::mod_car_spec(lp.car.type - rush2::car_types::first_mod);
            }
        }
        set_cars(players);
    }
    *stay = true;
    return true;
}

bool rush2::online::game::in_lobby() {
    State st = state();
    return st != State::Offline && st != State::Connecting;
}

bool rush2::online::game::car_select_back(uint8_t* rdram) {
    if (state() != State::Lobby || is_host()) {
        return false;
    }
    // Following the host back, or a player who picked going to change their car (the track select brings them back
    // in while the host's car select is up). A player who hasn't picked stays: the race waits for their car.
    return !allow_back && !car_chosen;
}
