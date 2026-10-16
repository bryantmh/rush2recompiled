#ifndef __ONLINE_INTERNAL_H__
#define __ONLINE_INTERNAL_H__

// Shared by the files of src/online/ (docs/online.md).

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "online.h"

namespace rush2::online {
    // Bumped whenever the messages below change; machines with different versions can't play together (it is part of
    // the compatibility key too, but the hello checks it first to give a clearer reason).
    constexpr uint32_t protocol_version = 4;

    // Messages, each starting with its type byte. Host -> client unless marked client -> host.
    enum class Msg : uint8_t {
        Hello = 1,        // client -> host: u32 protocol, str key, str name, car, u8 games, long str gameplay
        Welcome = 2,      // u8 slot, str lobby name, u8 max players
        Reject = 3,       // str reason
        Roster = 4,       // roster
        Settings = 5,     // settings
        SetCars = 6,      // client -> host: u8 count, count x (str name, car, bytes profile, bytes side)
        SetProgress = 7,  // client -> host: u8 progress, u32 custom track checked, u8 lacks it
        SetName = 8,      // client -> host: str
        Start = 9,        // u32 seed, u8 input delay, settings, roster, max_players x (bytes profile, bytes side)
        Loaded = 10,      // client -> host: u32 hash
        Go = 11,          // every machine has loaded: the race begins
        Input = 12,       // client -> host: u32 frame, u8 count, count x input (the machine's players in local order)
        FrameInputs = 13, // u32 frame, max_players inputs
        Hash = 14,        // client -> host: u32 frame, u32 hash
        Desync = 15,      // u32 frame: the machines' races differ; the race ends
        LoadMismatch = 16,// a machine set up a different race; the race ends
        Finished = 17,    // client -> host: the race is over on this machine
        SetGameplay = 18, // client -> host: long str
        MenuState = 19,   // u8 phase (MenuPhase), settings, s8 option cursor, s8 option top
    };

    // ENet channels.
    constexpr uint8_t channel_lobby = 0;
    constexpr uint8_t channel_inputs = 1;
    constexpr uint8_t channel_count = 2;

    class Writer {
    public:
        std::vector<uint8_t> data;
        void u8(uint8_t v) { data.push_back(v); }
        void u16(uint16_t v) { u8(uint8_t(v)); u8(uint8_t(v >> 8)); }
        void u32(uint32_t v) { u16(uint16_t(v)); u16(uint16_t(v >> 16)); }
        void str(const std::string& s) {
            size_t n = std::min<size_t>(s.size(), 255);
            u8(uint8_t(n));
            data.insert(data.end(), s.begin(), s.begin() + n);
        }
        // A string of up to 64 KB.
        void long_str(const std::string& s) {
            size_t n = std::min<size_t>(s.size(), 0xFFFF);
            u16(uint16_t(n));
            data.insert(data.end(), s.begin(), s.begin() + n);
        }
        void bytes(const std::vector<uint8_t>& b) {
            u16(uint16_t(std::min<size_t>(b.size(), 0xFFFF)));
            data.insert(data.end(), b.begin(), b.begin() + std::min<size_t>(b.size(), 0xFFFF));
        }
        void input(const PlayerInput& in) {
            u16(in.buttons);
            u8(uint8_t(in.stick_x));
            u8(uint8_t(in.stick_y));
            u8(in.gas);
            u8(in.brake);
            u8(in.battle);
        }
        void car(const CarChoice& c) {
            u8(c.type);
            for (uint8_t v : c.colors) {
                u8(v);
            }
            u8(c.automatic);
        }
        void settings(const RaceSettings& s) {
            u8(uint8_t(s.mode));
            u8(s.race_type);
            u8(s.start_kind);
            u8(s.track);
            for (uint8_t b : s.menu) {
                u8(b);
            }
            u8(s.needs_games);
            u8(s.battle_points);
            u8(s.battle_minutes);
            u8(s.race_battle);
            str(s.mod_track);
            str(s.mod_track_name);
            bytes(s.circuit);
        }
        void roster(const RosterEntry* entries) {
            for (int i = 0; i < max_players; i++) {
                const RosterEntry& e = entries[i];
                u8(uint8_t((e.used ? 1 : 0) | (e.connected ? 2 : 0) | (e.lacks_track ? 4 : 0)));
                u8(e.machine);
                u8(e.local_index);
                u8(e.progress);
                u32(e.track_checked);
                str(e.name);
                u16(e.ping_ms);
                car(e.car);
                u8(e.games);
                long_str(e.gameplay);
            }
        }
    };

    // Reads a message; any read past the end sets `bad` and returns zeros, so a short message is caught once at the end.
    class Reader {
    public:
        Reader(const uint8_t* data, size_t size) : data(data), size(size) {}
        bool bad = false;
        uint8_t u8() {
            if (at >= size) {
                bad = true;
                return 0;
            }
            return data[at++];
        }
        uint16_t u16() { uint16_t lo = u8(); return uint16_t(lo | (uint16_t(u8()) << 8)); }
        uint32_t u32() { uint32_t lo = u16(); return lo | (uint32_t(u16()) << 16); }
        std::string str() {
            size_t n = u8();
            if (at + n > size) {
                bad = true;
                return {};
            }
            std::string s(reinterpret_cast<const char*>(data + at), n);
            at += n;
            return s;
        }
        std::string long_str() {
            size_t n = u16();
            if (at + n > size) {
                bad = true;
                return {};
            }
            std::string s(reinterpret_cast<const char*>(data + at), n);
            at += n;
            return s;
        }
        std::vector<uint8_t> bytes() {
            size_t n = u16();
            if (at + n > size) {
                bad = true;
                return {};
            }
            std::vector<uint8_t> b(data + at, data + at + n);
            at += n;
            return b;
        }
        PlayerInput input() {
            PlayerInput in;
            in.buttons = u16();
            in.stick_x = int8_t(u8());
            in.stick_y = int8_t(u8());
            in.gas = u8();
            in.brake = u8();
            in.battle = u8();
            return in;
        }
        CarChoice car() {
            CarChoice c;
            c.type = u8();
            for (uint8_t& v : c.colors) {
                v = u8();
            }
            c.automatic = u8();
            return c;
        }
        RaceSettings settings() {
            RaceSettings s;
            s.mode = GameMode(u8());
            s.race_type = u8();
            s.start_kind = u8();
            s.track = u8();
            for (uint8_t& b : s.menu) {
                b = u8();
            }
            s.needs_games = u8();
            s.battle_points = u8();
            s.battle_minutes = u8();
            s.race_battle = u8();
            s.mod_track = str();
            s.mod_track_name = str();
            s.circuit = bytes();
            return s;
        }
        void roster(RosterEntry* entries) {
            for (int i = 0; i < max_players; i++) {
                RosterEntry& e = entries[i];
                uint8_t flags = u8();
                e.used = (flags & 1) != 0;
                e.connected = (flags & 2) != 0;
                e.lacks_track = (flags & 4) != 0;
                e.machine = u8();
                e.local_index = u8();
                if (e.machine >= max_players || e.local_index >= max_local_players) {
                    bad = true;
                }
                e.progress = u8();
                e.track_checked = u32();
                e.name = str();
                e.ping_ms = u16();
                e.car = car();
                e.games = u8();
                e.gameplay = long_str();
            }
        }

    private:
        const uint8_t* data;
        size_t size;
        size_t at = 0;
    };

    // A display name cut to max_name_length printable characters (other characters become spaces); "Player" if empty.
    std::string clean_name(const std::string& name);

    // LAN beacons (online_discovery.cpp). The host's net thread calls send_beacon about once a second, and
    // stop_beacons when the lobby closes: if beacons went out, it sends `closing` (a beacon with max_players 0), which
    // takes the lobby off the lists at once instead of when its beacons stop being heard.
    void send_beacon(const std::vector<uint8_t>& payload);
    void stop_beacons(const std::vector<uint8_t>& closing);
    // Ends the LAN lobby list's listener thread (started by lan_lobbies()); at exit, before its std::thread is destroyed.
    void stop_lan_listener();
    // Builds the beacon payload for a hosted lobby.
    std::vector<uint8_t> beacon_payload(const std::string& name, uint16_t port, int players, int max_players,
        const RaceSettings& settings, bool racing);

    // Router port mapping (online_upnp.cpp): runs in the background; status() is shown in the Online tab.
    void upnp_open(uint16_t port);
    void upnp_close();
    std::string upnp_status();
    std::string upnp_public_address();
}

#endif
