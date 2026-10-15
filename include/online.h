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
    constexpr int max_players = 8;
    constexpr uint16_t default_port = 47100;  // The host's port (UDP).
    constexpr uint16_t discovery_port = 47101; // LAN lobby beacons (UDP broadcast).
    constexpr int max_name_length = 16;

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
        uint8_t track = 0;         // The track select id (0x8010C3F0).
        std::array<uint8_t, 0x20> menu{};  // The track select's settings (0x800D5760: laps, drones, options).
        uint8_t needs_games = 0;   // The games_* ROMs every player needs for the track.
        uint8_t battle_points = 10;  // A battle's points to win and time limit (0 none): rush2::battle's options.
        uint8_t battle_minutes = 0;

        bool operator==(const RaceSettings&) const = default;
    };

    struct CarChoice {
        uint8_t type = 0;
        uint8_t colors[4] = { 0, 0, 0, 0 };  // Main, accent, stripe (0 none), stripe color.

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
    enum class MenuPhase : uint8_t { Lobby, SelectPlayer, Track, Car };
    struct MenuState {
        MenuPhase phase = MenuPhase::Lobby;
        RaceSettings settings;     // As the host's menus have them now.
        bool input_open = false;   // The host's track select takes input (everyone is on it).
    };
    // The host's controller in its track select, which the others' read-only track select follows.
    struct MenuInput {
        uint16_t buttons = 0;
        int8_t stick_x = 0;
        int8_t stick_y = 0;
    };

    struct RosterEntry {
        bool used = false;       // A player holds the slot.
        bool connected = false;  // Still connected (a dropped player's car coasts for the rest of the race).
        uint8_t progress = 0;    // Progress: how far through the host's menus the player is.
        std::string name;
        uint16_t ping_ms = 0;
        CarChoice car;
        uint8_t games = 0;       // The other games' ROMs the player has (games_* bits): what tracks all can race.
        // The player's settings that change the race (src/online_tab.cpp, "config/option=value" lines): a race starts
        // only when every player's match the host's.
        std::string gameplay;
    };

    constexpr uint8_t games_rush2049 = 1;  // San Francisco Rush 2049 (src/rush2049/rush2049_rom.cpp).
    constexpr uint8_t games_rush1 = 2;     // San Francisco Rush (src/rush1/rush1_rom.cpp).

    // A lobby heard on the LAN (online_discovery.cpp).
    struct LanLobby {
        std::string address;  // The host's IPv4 address.
        uint16_t port = default_port;
        std::string name;
        uint8_t players = 0;
        uint8_t max_players = 0;
        RaceSettings settings;
        bool compatible = false;  // Same build and ROMs as this machine.
        bool racing = false;      // Mid-race (can't be joined until it ends).
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
    // Why the last host, join or session ended, for the UI; empty if nothing went wrong.
    std::string last_error();
    // Status of the host's router port mapping (UPnP), for the UI.
    std::string router_status();
    // This machine's addresses, for the host to share.
    std::vector<std::string> local_addresses();
    std::string public_address();

    // Which other games' ROMs this machine has (games_* bits), sent to the host when joining.
    void set_local_games(uint8_t games);
    // This machine's settings that change the race (RosterEntry::gameplay).
    void set_local_gameplay(const std::string& gameplay);
    void set_display_name(const std::string& name);
    std::string display_name();
    void set_car(const CarChoice& car);
    // This machine's progress through the menus (RosterEntry::progress).
    void set_progress(Progress progress);
    // Host only: starts the race with these settings once every player has picked a car. Returns false (with
    // last_error) if it can't.
    bool start_race(const RaceSettings& settings);
    // Host: publishes its menu state and its controller for the frame. Others: the host's latest menu state (false
    // before any came), and its controller inputs in order (false when none are waiting).
    void publish_menu(const MenuState& state, const MenuInput& input);
    bool host_menu(MenuState* out);
    bool next_menu_input(MenuInput* out);

    // LAN lobbies heard in the last few seconds. Listening starts on first call.
    std::vector<LanLobby> lan_lobbies();

    // The compatibility key of this machine: two machines can play together only with equal keys.
    std::string compatibility_key();

    // Lockstep, for the game thread (online_game.cpp).
    // Whether an online race is running on this machine.
    bool racing();
    // The machine's own input for the frame `frame + input delay`. Called once per game frame.
    void submit_local_input(uint32_t frame, const PlayerInput& input);
    // Waits up to timeout_ms (negative: for good) for every player's input for `frame` and copies them to `out`
    // (max_players entries).
    enum class Wait { Ready, Timeout, Ended };  // Ended: the race ended (a disconnect from the host, or leave()).
    Wait wait_frame_inputs(uint32_t frame, PlayerInput* out, int timeout_ms = -1);
    // The number of game frames between sampling an input and applying it.
    uint32_t input_delay();
    // The race's seed and settings, fixed when it started.
    uint32_t race_seed();
    RaceSettings race_settings();
    std::vector<RosterEntry> race_roster();
    // Reports this machine's state hash for a frame; the host compares them (desync detection).
    void report_state_hash(uint32_t frame, uint32_t hash);
    // The machine finished loading the race; the race begins (wait_frame_inputs for frame 0 returns) once all have.
    void report_loaded(uint32_t hash);
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
        // In an online race: a player's battle weapon buttons for the frame. False otherwise.
        bool battle_buttons(int player, uint8_t* out);
        // The car select once this machine's players have picked their cars (src/players4.cpp's hook, func_803B9478 at
        // 0x803B96D4): true when online, with *stay true to keep the screen up (waiting for the others) and false to
        // let it start the race.
        bool car_select_done(uint8_t* rdram, recomp_context* ctx, bool* stay);
        // The car select when a player backs out (0x803B9564): true to stay on it (online, unless the host went back).
        bool car_select_back(uint8_t* rdram);
        bool start_from_car_select(uint8_t* rdram, recomp_context* ctx);
        // The game's controller read (main.cpp): online lobbies block ports or feed them the host's input. True if
        // it replaced the input.
        bool controller_override(int port, uint16_t* buttons, float* x, float* y);
        // Each game frame outside an online race (online_game.cpp): leads this machine through the host's menus.
        void lobby_frame(uint8_t* rdram);
        // Creates the lobby overlay (on the UI thread, at startup).
        void init_overlay();
        // This machine's player's controller (port 1, as the game would read it). False without one.
        bool read_local_controller(uint16_t* buttons, float* x, float* y);
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
}

#endif
