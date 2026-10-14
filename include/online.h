#ifndef __ONLINE_H__
#define __ONLINE_H__

#include <cstdint>
#include <string>
#include <vector>

// Online play (docs/online.md): up to 8 players on separate machines, on a LAN or over the internet by the host's
// address. Every machine runs the same race in lockstep: each game frame waits until every player's input for it has
// arrived, and the host relays the inputs (players connect only to the host). src/online/online_net.cpp holds the
// connections and the lobby, src/online/online_discovery.cpp the LAN lobby list, src/online/online_upnp.cpp the
// router port mapping and src/online/online_game.cpp what the game does differently in an online race.
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

        bool operator==(const PlayerInput&) const = default;
    };

    enum class GameMode : uint8_t { Race, Stunt, Battle };

    // What the host chooses for the next race. Each player's car is in their roster entry.
    struct RaceSettings {
        GameMode mode = GameMode::Race;
        uint8_t track = 0;    // The track select's index for the mode (src/online/online_game.cpp).
        uint8_t laps = 3;
        bool fill_with_ai = true;  // Empty car slots race as computer cars.
        // The host's gameplay settings for the race (src/online/online_game.cpp: options that change the race).
        std::vector<uint8_t> gameplay;

        bool operator==(const RaceSettings&) const = default;
    };

    struct CarChoice {
        uint8_t type = 0;
        uint8_t colors[4] = { 0, 0, 0, 0 };  // Main, accent, stripe (0 none), stripe color.

        bool operator==(const CarChoice&) const = default;
    };

    struct RosterEntry {
        bool used = false;       // A player holds the slot.
        bool connected = false;  // Still connected (a dropped player's car coasts for the rest of the race).
        bool ready = false;
        std::string name;
        uint16_t ping_ms = 0;
        CarChoice car;
        uint8_t games = 0;       // The other games' ROMs the player has (games_* bits): what tracks all can race.
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
    RaceSettings settings();
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
    void set_display_name(const std::string& name);
    std::string display_name();
    void set_car(const CarChoice& car);
    void set_ready(bool ready);
    // Host only.
    void set_settings(const RaceSettings& settings);
    // Host only: starts the race once every player is ready. Returns false (with last_error) if it can't.
    bool start_race();

    // LAN lobbies heard in the last few seconds. Listening starts on first call.
    std::vector<LanLobby> lan_lobbies();

    // The compatibility key of this machine: two machines can play together only with equal keys.
    std::string compatibility_key();

    // Lockstep, for the game thread (online_game.cpp).
    // Whether an online race is running on this machine.
    bool racing();
    // The machine's own input for the frame `frame + input delay`. Called once per game frame.
    void submit_local_input(uint32_t frame, const PlayerInput& input);
    // Waits until every player's input for `frame` has arrived and copies them to `out` (max_players entries).
    // Returns false if the race ended (a disconnect from the host, or leave()).
    bool wait_frame_inputs(uint32_t frame, PlayerInput* out);
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
    }
}

#endif
