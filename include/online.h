#ifndef __ONLINE_H__
#define __ONLINE_H__

#include <array>
#include <cstdint>
#include <string>
#include <vector>

// Online play (docs/online.md): up to 8 players on separate machines, on a LAN or over the internet by the host's
// address. Every machine runs the same race in lockstep: each game frame waits until every player's input for it has
// arrived, and the host relays the inputs (players connect only to the host). src/online/online_net.cpp holds the
// connections and the lobby, src/online/online_discovery.cpp the LAN lobby list, src/online/online_upnp.cpp the
// router port mapping and src/online/online_game.cpp what the game does differently in an online race.
#include "recomp.h"

namespace rush2::online {
    // A line of the online log: the console, and this game run's online/session_<date>_<time>.txt in the data folder
    // (the last 20 runs' kept), so a kick or a failed join leaves a record to read after (printf format, newline
    // included). Any thread.
    void session_log(const char* format, ...);

    constexpr int max_players = 8;
    // Players on one machine (split screen): a machine holds a slot per local player (RosterEntry::machine).
    constexpr int max_local_players = 4;
    constexpr uint16_t default_port = 47100;  // The host's port (UDP).
    constexpr uint16_t discovery_port = 47101; // LAN lobby beacons (UDP broadcast).
    constexpr int max_name_length = 16;
    // Mod car slots (rush2::car_types::count_mod, types 57-72).
    constexpr int max_mod_cars = 16;

    // One player's controls for one game frame, as every machine applies them.
    struct PlayerInput {
        uint16_t buttons = 0;  // N64 buttons in the game's race layout (src/controls.cpp).
        int8_t stick_x = 0;
        int8_t stick_y = 0;
        uint8_t gas = 0;       // Analog pedals (src/input.cpp), 0-255 of a pressed pedal.
        uint8_t brake = 0;
        uint8_t battle = 0;    // The battle's weapon buttons (rush2::controls::battle_buttons bits).

        bool operator==(const PlayerInput&) const = default;
    };

    // Stunt and battle are what the track is: Rush 2's stunt track and the Rush 2049 arenas race in stunt mode.
    enum class GameMode : uint8_t { Race, Stunt, Battle };

    // The race the host chose in its own menus (Start Game, the track select): every machine sets up that race.
    struct RaceSettings {
        GameMode mode = GameMode::Race;
        uint8_t race_type = 0;     // 0x8010C3E8 as the host's Start Game menu left it.
        uint8_t start_kind = 0;    // What the host chose on Start Game (rush2::track2049::StartKind).
        uint8_t track = 0;         // The track select id (0x8010C3F0).
        std::array<uint8_t, 0x20> menu{};  // The track select's settings (0x800D5760: laps, drones, options).
        uint8_t needs_games = 0;   // The games_* ROMs every player needs for the track.
        uint8_t battle_points = 10;  // A battle's points to win and time limit (0 none): rush2::battle's options.
        uint8_t battle_minutes = 0;
        uint8_t race_battle = 0;     // The race track select's BATTLE row (rush2::battle::race_battle): weapons.
        // A custom track (src/track_mod/) is known by what it is, not its id: each machine numbers its custom tracks
        // (ids 38-63) in its own mod order. mod_track is rush2::track2049::online_track_key ("" for the other
        // tracks), mod_track_name its name for the lobby's messages.
        std::string mod_track;
        std::string mod_track_name;
        // A circuit (race_type 1): the host's circuit as game::read_circuit gives it, which every other machine races
        // in place of its own profile's (docs/online.md#circuits). Empty for the other races.
        std::vector<uint8_t> circuit;

        bool operator==(const RaceSettings&) const = default;
    };

    struct CarChoice {
        uint8_t type = 0;
        uint8_t colors[4] = { 0, 0, 0, 0 };  // Main, accent, stripe (0 none), stripe color.
        // The car select's TRANSMISSION (player record +0x20: 1 automatic, 0 manual), which the race copies to the
        // car's gearbox (race_setup_cars_8009E6DC at 0x8009EA84, physics car +0x3D8).
        uint8_t automatic = 1;
        // The player's Rush 2049 wing style (rush2::wings::saved_style, 0-2), which their car's wings fly with on
        // every machine (src/rush2049/wings_state.cpp).
        uint8_t wing_style = 0;

        bool operator==(const CarChoice&) const = default;
    };

    // How far a player is through the menus the host leads them through (src/online/online_lobby.cpp).
    enum class Progress : uint8_t {
        Lobby,           // Waiting for the host to go to Select Player.
        ChoosingPlayer,  // On Select Player.
        TrackSelect,     // Has a player: on the track select (the host's, read-only for the others).
        ChoosingCar,     // On the car select.
        CarChosen,       // Picked a car: waiting for the others.
    };

    // Where the host is in its menus, published every frame for the others to follow.
    // Circuit: the host is on the circuit menu (CONTINUE, NEW CIRCUIT; front-end screens 0xA and 0xB) after Select Player.
    enum class MenuPhase : uint8_t { Lobby, SelectPlayer, Track, Car, Circuit };
    struct MenuState {
        MenuPhase phase = MenuPhase::Lobby;
        RaceSettings settings;     // As the host's menus have them now.
        // The host's track select as it shows it, which the others' track selects copy: the option row under the
        // cursor (0x803D05BC) and the top row on screen (0x803D05C4).
        int8_t option_cursor = 0;
        int8_t option_top = 1;
        // The tracks the host's track select offers (rush2::track2049::offered_tracks, in its ids), which the others'
        // offer too; empty until the host has been on its track select.
        std::vector<uint8_t> offered_tracks;
        // Why the host's race can't start yet (start_race's last_error), shown on every machine so the player it names
        // knows what to do (pick another car, close a menu); empty when nothing holds it.
        std::string start_error;
    };

    struct RosterEntry {
        bool used = false;       // A player holds the slot.
        bool connected = false;  // Still connected (a dropped player's car coasts for the rest of the race).
        // The machine the player plays on, as the slot of its first player, and which of its local players this is
        // (0 the machine's own slot, 1-3 the players who joined on its other controllers: set_cars). A race orders its
        // players by machine, so each machine's players drive neighboring cars (online_game.cpp's setup_race).
        uint8_t machine = 0;
        uint8_t local_index = 0;
        uint8_t progress = 0;    // Progress: how far through the host's menus the player is.
        std::string name;
        uint16_t ping_ms = 0;
        CarChoice car;
        uint8_t games = 0;       // The other games' ROMs the player has (games_* bits): what tracks all can race.
        // The custom track the player last checked for (track_check of the host's RaceSettings::mod_track, 0 none) and
        // whether they lack it (set_track_check): the host's track select confirms a custom track only once everyone
        // has checked that one and has it.
        uint32_t track_checked = 0;
        bool lacks_track = false;
        // The mod cars the player's machine has, by mod car slot (rush2::car_mods::online_car_checks: a hash of the mod,
        // the car and its files; 0 an empty slot). A mod car races online only when every machine has the same car in
        // the same slot (start_race), so its type number and options mean the same everywhere.
        std::array<uint32_t, max_mod_cars> mod_cars{};
        // Host's roster: the player is in the race under way (a player who joined during it waits for the next one).
        bool in_race = false;
        // The player's settings that change the race (src/online_tab.cpp, "config/option=value" lines). The others'
        // games take the host's (rush2::online_tab::follow_host_settings).
        std::string gameplay;
        // The save profile the player races with (0x6C0 bytes, the record the car select left: car type, keys and
        // the like), sent with the car (set_cars) and in the race start only, not in every roster.
        std::vector<uint8_t> profile;
        // The profile's Rush 2049 car options (rush2::car2049::side_block), sent the same way: every machine's race
        // reads the player's 2049 car setup from them.
        std::vector<uint8_t> side;
        // A mod car's spec (rush2::car2049::mod_car_spec: its physics), sent the same way, so a machine without the car
        // races it with the same physics (docs/online.md#mod-cars). Empty for the other cars.
        std::vector<uint8_t> mod_spec;
    };

    // One of this machine's players as the car select left them (set_cars).
    struct LocalPlayer {
        std::string name;  // The profile's name, or "" (the lobby then numbers the machine's name).
        CarChoice car;
        std::vector<uint8_t> profile;
        std::vector<uint8_t> side;
        std::vector<uint8_t> mod_spec;
    };

    constexpr uint8_t games_rush2049 = 1;  // San Francisco Rush 2049 (src/rush2049/rush2049_rom.cpp).
    constexpr uint8_t games_rush1 = 2;     // San Francisco Rush (src/rush1/rush1_rom.cpp).
    constexpr uint8_t games_calspeed = 4;  // California Speed (src/calspeed/calspeed_rom.cpp).

    // A lobby heard on the LAN (online_discovery.cpp).
    struct LanLobby {
        std::string address;  // The host's IPv4 address.
        uint16_t port = default_port;
        std::string name;
        uint8_t players = 0;
        uint8_t max_players = 0;
        RaceSettings settings;
        bool compatible = false;  // Same build and ROMs as this machine.
        bool racing = false;      // Mid-race (a player who joins waits for the next race).
    };

    enum class State {
        Offline,     // No lobby.
        Connecting,  // Joining a host.
        Lobby,       // In a lobby (hosting or joined).
        Loading,     // The race is starting: every machine sets it up and reports when loaded.
        Racing,      // In a lockstep race.
    };

    struct HostOptions {
        std::string lobby_name;
        uint16_t port = default_port;
        int max_players = rush2::online::max_players;
        bool open_router_port = true;  // UPnP (online_upnp.cpp).
    };

    // Whether online play could start (the networking library initialized).
    bool available();

    // Lobby control (UI thread).
    bool host(const HostOptions& options);
    // address is "a.b.c.d" or "a.b.c.d:port" or a host name.
    bool join(const std::string& address);
    void leave();
    State state();
    bool is_host();
    // The slot (0-7) this machine plays in, or -1.
    int local_slot();
    std::vector<RosterEntry> roster();
    int max_players_setting();
    std::string lobby_name();
    // The port of the lobby this machine hosts or joined.
    uint16_t lobby_port();
    // Why the last host, join or session ended, for the UI; empty if nothing went wrong.
    std::string last_error();
    // Status of the host's router port mapping (UPnP), for the UI: router_status() is the full explanation,
    // router_port() what it came to.
    std::string router_status();
    enum class RouterPort {
        None,         // Not hosting, or not asked.
        Checking,     // Asking the router.
        Open,         // Forwarded: players on the internet can join.
        Unreachable,  // Forwarded, but the router is behind another network (carrier-grade NAT).
        Closed,       // No router answered or it refused: the host has to forward the port by hand.
    };
    RouterPort router_port();
    // This machine's addresses, for the host to share.
    std::vector<std::string> local_addresses();
    std::string public_address();

    // Which other games' ROMs this machine has (games_* bits), sent to the host when joining.
    void set_local_games(uint8_t games);
    // This machine's mod cars (RosterEntry::mod_cars), sent when joining and whenever they change.
    void set_local_mod_cars(const std::array<uint32_t, max_mod_cars>& checks);
    // This machine's settings that change the race (RosterEntry::gameplay).
    void set_local_gameplay(const std::string& gameplay);
    void set_display_name(const std::string& name);
    std::string display_name();
    // The name of the save profile this machine's player picked on Select Player, which the lobby shows in place of
    // the display name; empty for none.
    void set_profile_name(const std::string& name);
    // This machine's players (1-4) and their cars, once its car select has them all: the host gives the machine a slot
    // per player (or as many as the lobby has room for: start_race says so).
    void set_cars(const std::vector<LocalPlayer>& players);
    // This machine's progress through the menus (RosterEntry::progress).
    void set_progress(Progress progress);
    // The custom track this machine checked for (track_check of its key) and whether it lacks it
    // (RosterEntry::track_checked, lacks_track).
    void set_track_check(uint32_t checked, bool lacks);
    // A custom track key's short hash for the roster (0 for "").
    uint32_t track_check(const std::string& mod_track);
    // Host only: starts the race with these settings once every player has picked a car. Returns false (with
    // last_error) if it can't.
    bool start_race(const RaceSettings& settings);
    // Host: publishes its menu state for the frame. Others: the host's latest menu state (false before any came).
    void publish_menu(const MenuState& state);
    bool host_menu(MenuState* out);

    // LAN lobbies heard in the last few seconds. Listening starts on first call.
    std::vector<LanLobby> lan_lobbies();

    // The compatibility key of this machine: two machines can play together only with equal keys.
    std::string compatibility_key();

    // Lockstep, for the game thread (online_game.cpp).
    // Whether an online race is running on this machine.
    bool racing();
    // The machine's own players' inputs (in local order) for the frame `frame + input delay`. Called once per game frame.
    void submit_local_input(uint32_t frame, const std::vector<PlayerInput>& inputs);
    // Waits up to timeout_ms (negative: for good) for every player's input for `frame` and copies them to `out`
    // (max_players entries, by slot). *resync is set when the frame starts with a resync (below).
    enum class Wait { Ready, Timeout, Ended };  // Ended: the race ended (a disconnect from the host, or leave()).
    Wait wait_frame_inputs(uint32_t frame, PlayerInput* out, int timeout_ms = -1, bool* resync = nullptr);
    // Resyncs (docs/online.md#resyncs): when the machines' races differ (a state hash or the load check), the host
    // marks a frame no machine has run yet. At its start the host sends its race state (send_snapshot), and every other
    // machine waits for it (wait_snapshot) and takes it, so the race goes on in step instead of ending.
    void send_snapshot(uint32_t frame, const std::vector<uint8_t>& state);
    Wait wait_snapshot(uint32_t frame, std::vector<uint8_t>* out, int timeout_ms);
    // The number of game frames between sampling an input and applying it.
    uint32_t input_delay();
    // The race's seed and settings, fixed when it started.
    uint32_t race_seed();
    RaceSettings race_settings();
    std::vector<RosterEntry> race_roster();
    // Reports this machine's state hash for a frame; the host compares them (desync detection).
    void report_state_hash(uint32_t frame, uint32_t hash);
    // The machine finished loading the race; the race begins (wait_frame_inputs for frame 0 returns) once all have.
    // hash is its starting state (state_hash: a difference resyncs), track and cars what it set up (the track's
    // collision data, the cars' physics tables: a difference can't be repaired, and the race doesn't start).
    void report_loaded(uint32_t hash, uint32_t track, uint32_t cars);
    // The race is over on this machine: back to the lobby.
    void race_finished();
    // A message for the race's overlay ("Waiting for ..."), empty for none.
    std::string race_status();

    // Called each frame by the UI to pump events that must run on the UI side (none yet); safe to call any time.
    void update();

    // The game side (src/online/online_game.cpp).
    namespace game {
        // Where this machine's player's input comes from (main.cpp's controller callback, with input scripts).
        void set_input_source(bool (*source)(int port, uint16_t* buttons, float* x, float* y));
        // In an online race's physics tick: a player's pedal pressures (0-1) for the frame. False otherwise.
        bool pedals(int player, float* gas, float* brake);
        // An online race is set up or running on this machine.
        bool in_race();
        // Whether every machine of the race has the game's ROM (games_* bit), so a race car of that game (a computer
        // car's random type) can be on every machine. True outside an online race.
        bool race_has_game(uint8_t game);
        // In an online race: the car type player (car slot) `car` picked, which the race gives their car whatever its
        // profile says (src/rush2049/car2049.cpp, rush2_car49_select_get). False otherwise.
        bool car_type(int car, int* type);
        // In an online race: the wing style player (car slot) `car` picked (CarChoice::wing_style). False otherwise.
        bool wing_style(int car, int* style);
        // This machine's mod cars as the lobby sees them (rush2::car_mods::online_car_checks; a test aid can fake them).
        std::array<uint32_t, max_mod_cars> mod_car_checks();
        // In an online race: this machine's first player (car slot), whose record has their own profile; -1 otherwise.
        // A machine's players drive neighboring cars: local_race_player() + i for its player i (local_car).
        int local_race_player();
        // In an online race: how many players this machine has (1-4, its views); 0 otherwise.
        int local_race_players();
        // In an online race: the car slot of this machine's player i (view i), or -1.
        int local_car(int i);
        // In an online race: the number of human players (cars 0 to race_players() - 1); 0 otherwise.
        int race_players();
        // In an online race: the car whose engine Rush 2's engine sound slot `slot` (0-1, the local players 1 and 2's)
        // plays, this machine's player `slot`'s, or -1 for none. False outside one (each slot is its record's car).
        bool engine_slot_car(int slot, int* car);
        // The controller port a human car's rumble goes to, from its player record and the record's port: online this
        // machine's player's is port 0 (its controller, read_local_controller) and the others' none (5).
        int rumble_port(uint32_t record, int port);
        // Around the race HUD's setup and its update and drawing (src/hud.cpp, rush2_online_present_begin): online the
        // player count (0x8010C3E2) reads 1 there, so the HUD is the one player's of a one-player race. Calls nest.
        void hud_players_begin(uint8_t* rdram);
        void hud_players_end(uint8_t* rdram);
        // The race HUD's setup has built its elements (src/hud.cpp): online the gear box's and the tachometer's are set
        // to this machine's player (rush2_online_hud_views).
        void hud_built(uint8_t* rdram);
        // In an online race: a player's battle weapon buttons for the frame. False otherwise.
        bool battle_buttons(int player, uint8_t* out);
        // The car select once this machine's players have picked their cars (src/players4.cpp's hook, func_803B9478 at
        // 0x803B96D4): true when online, with *stay true to keep the screen up (waiting for the others) and false to
        // let it start the race.
        bool car_select_done(uint8_t* rdram, recomp_context* ctx, bool* stay);
        // The car select when a player backs out (0x803B9564): true to stay on it (online, unless the host went back).
        bool car_select_back(uint8_t* rdram);
        bool start_from_car_select(uint8_t* rdram, recomp_context* ctx);
        // The game's controller read (main.cpp): online lobbies block ports or press buttons for the player. True if
        // it replaced the input.
        bool controller_override(int port, uint16_t* buttons, float* x, float* y);
        // Each game frame outside an online race (online_game.cpp): leads this machine through the host's menus.
        void lobby_frame(uint8_t* rdram);
        // The race is over on this machine (its post-race next): the lobby takes it back to the track select.
        void race_over();
        // In a race: what it waits for ("Waiting for ...", race_status), shown on the lobby overlay; empty for nothing.
        void race_notice(const std::string& text);
        // Creates the lobby overlay (on the UI thread, at startup).
        void init_overlay();
        // This machine's player's controller (port 1, as the game would read it). False without one.
        bool read_local_controller(uint16_t* buttons, float* x, float* y);
        // Whether this machine is in a lobby or an online race (online_lobby.cpp steers its menus).
        bool in_lobby();

        // Online circuits (src/online/online_game.cpp, docs/online.md#circuits). The host races its profile's circuit;
        // every other machine races a copy of it in spare RDRAM in place of its own profile's, so its saved circuit is
        // left alone. read_circuit gives this machine's circuit for RaceSettings::circuit (empty outside a circuit);
        // follow_circuit puts the host's in place (the circuit block, races, standings and race index).
        std::vector<uint8_t> read_circuit(uint8_t* rdram);
        void follow_circuit(uint8_t* rdram, const std::vector<uint8_t>& circuit);
        // The followed circuit's length in races, or -1 when this machine races its own circuit (src/circuit.cpp).
        int circuit_length();
    }
}

namespace recomp::config {
    class Config;
}

// The Online tab (src/online_tab.cpp).
namespace rush2::online_tab {
    // Registers the options and the tab. Call before recompui::config::finalize().
    void create_tab();
    // Loads online.json and hands this machine's choices to the session. Call after every other config has loaded.
    void load_config();
    // The "online" config (online.json): the display name, which the Controls tab also shows, and the tab's choices.
    recomp::config::Config& config();
    // Game thread, every frame: in a joined lobby this machine's gameplay settings follow the host's (kept in memory,
    // with this machine's own backed up in the save file); out of one they are put back. Changes wait while a menu is
    // open (the configs are the UI's then).
    void follow_host_settings();
}

#endif
