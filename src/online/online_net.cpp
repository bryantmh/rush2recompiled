// Online play: the connections, the lobby and the lockstep input relay (docs/online.md). ENet over UDP, IPv4.
//
// Topology: every player connects only to the host. The host owns the lobby (roster, race settings), checks each
// joiner's compatibility key, and in a race combines every player's input for a frame into one message it sends to
// everyone, itself included: a frame runs on every machine with exactly the same inputs.
//
// Threads: ENet objects are touched only by the net thread (net_loop). The UI and game threads change the shared
// state under `state_mutex` and queue messages in `outbox`, which the net thread sends. The game thread blocks in
// wait_frame_inputs on `inputs_cv` until the net thread has the frame.
//
// Lockstep: the game calls submit_local_input(f + delay, input) and then wait_frame_inputs(f) every frame. A client
// sends its input to the host; the host keeps each slot's inputs until it has every connected player's for the next
// frame, then broadcasts the frame. Frames before `delay` have no inputs (everyone coasts) so the first real ones
// have time to arrive. A player who drops coasts for the rest of the race, from the first frame the host hadn't
// broadcast yet, the same on every machine.

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <mutex>
#include <optional>
#include <random>
#include <thread>

#include "enet/enet.h"

#include "online_internal.h"

#ifndef RUSH2_VERSION
#define RUSH2_VERSION "dev"
#endif
#ifndef RUSH2_BUILD_ID
#define RUSH2_BUILD_ID "unknown"
#endif

using namespace rush2::online;

namespace {
    using clock_type = std::chrono::steady_clock;

    // Timings.
    constexpr auto connect_timeout = std::chrono::seconds(6);
    constexpr auto beacon_period = std::chrono::seconds(1);
    constexpr auto roster_period = std::chrono::seconds(1);
    constexpr auto slow_player_notice = std::chrono::seconds(1);  // Show "Waiting for ..." after this.
    constexpr auto slow_player_drop = std::chrono::seconds(15);   // Then drop the player.
    constexpr uint32_t frame_ms = 33;  // A game frame (30 fps): the unit of the input delay.
    constexpr int max_input_delay = 8;

    bool enet_ready = false;

    std::mutex state_mutex;
    std::condition_variable inputs_cv;

    // Lobby state.
    State cur_state = State::Offline;
    bool hosting = false;
    int my_slot = -1;
    RosterEntry slots[max_players];
    RaceSettings cur_settings;
    int max_players_opt = max_players;
    std::string cur_lobby_name;
    std::string error_text;
    std::string my_name = "Player";
    CarChoice my_car;
    uint8_t my_games = 0;
    uint16_t host_port = default_port;

    // Messages for the net thread to send. Target: a slot (host), to_host (client) or to_all (host).
    constexpr int to_host = -1;
    constexpr int to_all = -2;
    struct Outgoing {
        int target;
        uint8_t channel;
        std::vector<uint8_t> data;
    };
    std::vector<Outgoing> outbox;

    // The race.
    uint32_t seed = 0;
    uint32_t delay = 1;
    RaceSettings race_set;
    RosterEntry race_slots[max_players];
    bool go = false;          // Every machine loaded: frame inputs flow.
    bool race_over = false;   // The session ended under the race (wait_frame_inputs returns false).
    std::map<uint32_t, std::array<PlayerInput, max_players>> frames;
    clock_type::time_point waiting_since;
    bool waiting = false;
    // Host only.
    std::map<uint32_t, PlayerInput> pending[max_players];
    uint32_t next_broadcast = 0;
    clock_type::time_point last_progress;
    bool loaded[max_players];
    uint32_t loaded_hash[max_players];
    std::map<uint32_t, std::array<std::optional<uint32_t>, max_players>> hashes;

    // Net thread.
    std::thread net_thread;
    std::atomic<bool> net_running = false;
    ENetHost* enet_host = nullptr;
    ENetPeer* host_peer = nullptr;            // Client: the connection to the host.
    ENetPeer* slot_peers[max_players] = {};   // Host: each client's connection.
    clock_type::time_point connect_started;
    clock_type::time_point last_beacon;
    clock_type::time_point last_roster;

    void queue(int target, uint8_t channel, std::vector<uint8_t> data) {
        outbox.push_back({ target, channel, std::move(data) });
    }

    std::vector<uint8_t> roster_message() {
        Writer w;
        w.u8(uint8_t(Msg::Roster));
        w.roster(slots);
        return w.data;
    }

    std::vector<uint8_t> settings_message() {
        Writer w;
        w.u8(uint8_t(Msg::Settings));
        w.settings(cur_settings);
        return w.data;
    }

    int used_count() {
        int n = 0;
        for (const RosterEntry& e : slots) {
            n += e.used ? 1 : 0;
        }
        return n;
    }

    void clear_race() {
        go = false;
        race_over = false;
        frames.clear();
        waiting = false;
        for (int i = 0; i < max_players; i++) {
            pending[i].clear();
            loaded[i] = false;
            loaded_hash[i] = 0;
        }
        hashes.clear();
        next_broadcast = 0;
    }

    // Ends the race on this machine with a reason (the lobby stays, unless the connection itself is gone).
    void end_race(const std::string& reason) {
        if (cur_state == State::Loading || cur_state == State::Racing) {
            error_text = reason;
            race_over = true;
            inputs_cv.notify_all();
        }
    }

    void reset_session() {
        cur_state = State::Offline;
        hosting = false;
        my_slot = -1;
        for (RosterEntry& e : slots) {
            e = RosterEntry{};
        }
        outbox.clear();
        race_over = true;
        inputs_cv.notify_all();
    }

    // Net thread helpers (state_mutex held).
    void send_packet(ENetPeer* peer, uint8_t channel, const std::vector<uint8_t>& data) {
        if (peer == nullptr) {
            return;
        }
        ENetPacket* packet = enet_packet_create(data.data(), data.size(), ENET_PACKET_FLAG_RELIABLE);
        if (enet_peer_send(peer, channel, packet) != 0) {
            enet_packet_destroy(packet);
        }
    }

    void flush_outbox() {
        for (Outgoing& o : outbox) {
            if (o.target == to_host) {
                send_packet(host_peer, o.channel, o.data);
            }
            else if (o.target == to_all) {
                for (ENetPeer* p : slot_peers) {
                    send_packet(p, o.channel, o.data);
                }
            }
            else if (o.target >= 0 && o.target < max_players) {
                send_packet(slot_peers[o.target], o.channel, o.data);
            }
        }
        outbox.clear();
    }

    int slot_of(ENetPeer* peer) {
        for (int i = 0; i < max_players; i++) {
            if (slot_peers[i] == peer) {
                return i;
            }
        }
        return -1;
    }

    // Host: the slots whose input a frame needs: used, still connected, human.
    bool needs_input(int slot) {
        return race_slots[slot].used && slots[slot].connected;
    }

    // Host: broadcasts every frame whose inputs are all in.
    void host_relay() {
        if (!go || cur_state != State::Racing) {
            return;
        }
        while (true) {
            uint32_t f = next_broadcast;
            std::array<PlayerInput, max_players> inputs{};
            bool complete = true;
            for (int i = 0; i < max_players && complete; i++) {
                if (f < delay || !needs_input(i)) {
                    continue;
                }
                auto it = pending[i].find(f);
                if (it == pending[i].end()) {
                    complete = false;
                }
                else {
                    inputs[i] = it->second;
                }
            }
            if (!complete) {
                break;
            }
            for (int i = 0; i < max_players; i++) {
                pending[i].erase(f);
            }
            Writer w;
            w.u8(uint8_t(Msg::FrameInputs));
            w.u32(f);
            for (const PlayerInput& in : inputs) {
                w.input(in);
            }
            for (ENetPeer* p : slot_peers) {
                send_packet(p, channel_inputs, w.data);
            }
            frames[f] = inputs;
            next_broadcast = f + 1;
            last_progress = clock_type::now();
            inputs_cv.notify_all();
        }

        // A player whose input hasn't come for a long time is dropped (their car coasts from the next frame).
        if (clock_type::now() - last_progress > slow_player_drop) {
            for (int i = 1; i < max_players; i++) {
                if (needs_input(i) && pending[i].find(next_broadcast) == pending[i].end() && slot_peers[i] != nullptr) {
                    enet_peer_disconnect_now(slot_peers[i], 0);
                    slot_peers[i] = nullptr;
                    slots[i].connected = false;
                    std::fprintf(stderr, "[online] dropped player %d (no input for frame %u)\n", i + 1, next_broadcast);
                }
            }
            last_progress = clock_type::now();
        }
    }

    // Host: every machine reported loaded: compares their starting states and lets the race begin.
    void host_check_loaded() {
        if (cur_state != State::Loading || go) {
            return;
        }
        std::optional<uint32_t> reference;
        for (int i = 0; i < max_players; i++) {
            if (!needs_input(i)) {
                continue;
            }
            if (!loaded[i]) {
                return;
            }
            if (!reference) {
                reference = loaded_hash[i];
            }
            else if (*reference != loaded_hash[i]) {
                Writer w;
                w.u8(uint8_t(Msg::LoadMismatch));
                for (ENetPeer* p : slot_peers) {
                    send_packet(p, channel_lobby, w.data);
                }
                std::fprintf(stderr, "[online] player %d set up a different race (hash %08X, expected %08X)\n", i + 1,
                    loaded_hash[i], *reference);
                end_race("The race couldn't start: player " + std::to_string(i + 1) + "'s game set it up differently.");
                return;
            }
        }
        go = true;
        cur_state = State::Racing;
        last_progress = clock_type::now();
        Writer w;
        w.u8(uint8_t(Msg::Go));
        for (ENetPeer* p : slot_peers) {
            send_packet(p, channel_lobby, w.data);
        }
        inputs_cv.notify_all();
    }

    // Host: compares the state hashes reported for a frame once every machine has.
    void host_check_hashes() {
        for (auto it = hashes.begin(); it != hashes.end();) {
            const auto& per_slot = it->second;
            bool all = true;
            std::optional<uint32_t> reference;
            int differs = -1;
            for (int i = 0; i < max_players; i++) {
                if (!needs_input(i)) {
                    continue;
                }
                if (!per_slot[i]) {
                    all = false;
                    break;
                }
                if (!reference) {
                    reference = per_slot[i];
                }
                else if (*per_slot[i] != *reference && differs < 0) {
                    differs = i;
                }
            }
            if (!all) {
                ++it;
                continue;
            }
            if (differs >= 0) {
                uint32_t frame = it->first;
                Writer w;
                w.u8(uint8_t(Msg::Desync));
                w.u32(frame);
                for (ENetPeer* p : slot_peers) {
                    send_packet(p, channel_lobby, w.data);
                }
                std::fprintf(stderr, "[online] desync at frame %u (player %d)\n", frame, differs + 1);
                end_race("The race went out of sync at frame " + std::to_string(frame) + " (player " +
                    std::to_string(differs + 1) + "). Its log is in the online folder.");
                hashes.clear();
                return;
            }
            it = hashes.erase(it);
        }
    }

    void host_receive(int slot, ENetPeer* peer, Reader& r, Msg type) {
        switch (type) {
            case Msg::Hello: {
                uint32_t protocol = r.u32();
                std::string key = r.str();
                std::string name = clean_name(r.str());
                CarChoice car = r.car();
                uint8_t games = r.u8();
                if (r.bad) {
                    return;
                }
                auto reject = [&](const std::string& reason) {
                    Writer w;
                    w.u8(uint8_t(Msg::Reject));
                    w.str(reason);
                    send_packet(peer, channel_lobby, w.data);
                    enet_peer_disconnect_later(peer, 0);
                };
                if (protocol != protocol_version || key != compatibility_key()) {
                    reject("Your game is a different version than the host's (host: " + compatibility_key() +
                        ", yours: " + key + "). Both need the same release.");
                    return;
                }
                if (cur_state != State::Lobby) {
                    reject("The lobby is in a race. Join again when it ends.");
                    return;
                }
                int free = -1;
                if (used_count() < max_players_opt) {
                    for (int i = 0; i < max_players; i++) {
                        if (!slots[i].used) {
                            free = i;
                            break;
                        }
                    }
                }
                if (free < 0) {
                    reject("The lobby is full.");
                    return;
                }
                std::fprintf(stderr, "[online] %s joined as player %d\n", name.c_str(), free + 1);
                slot_peers[free] = peer;
                RosterEntry& e = slots[free];
                e = RosterEntry{};
                e.used = true;
                e.connected = true;
                e.name = name;
                e.car = car;
                e.games = games;
                Writer w;
                w.u8(uint8_t(Msg::Welcome));
                w.u8(uint8_t(free));
                w.str(cur_lobby_name);
                w.u8(uint8_t(max_players_opt));
                send_packet(peer, channel_lobby, w.data);
                send_packet(peer, channel_lobby, settings_message());
                queue(to_all, channel_lobby, roster_message());
                break;
            }
            case Msg::SetCar:
                if (slot >= 0) {
                    CarChoice car = r.car();
                    if (!r.bad && cur_state == State::Lobby) {
                        slots[slot].car = car;
                        queue(to_all, channel_lobby, roster_message());
                    }
                }
                break;
            case Msg::SetReady:
                if (slot >= 0) {
                    bool ready = r.u8() != 0;
                    if (!r.bad && cur_state == State::Lobby) {
                        slots[slot].ready = ready;
                        queue(to_all, channel_lobby, roster_message());
                    }
                }
                break;
            case Msg::SetName:
                if (slot >= 0) {
                    std::string name = clean_name(r.str());
                    if (!r.bad) {
                        slots[slot].name = name;
                        queue(to_all, channel_lobby, roster_message());
                    }
                }
                break;
            case Msg::Loaded:
                if (slot >= 0 && cur_state == State::Loading) {
                    uint32_t hash = r.u32();
                    if (!r.bad) {
                        loaded[slot] = true;
                        loaded_hash[slot] = hash;
                        host_check_loaded();
                    }
                }
                break;
            case Msg::Input:
                // A machine sends its first inputs while the others may still be loading.
                if (slot >= 0 && (cur_state == State::Racing || cur_state == State::Loading)) {
                    uint32_t frame = r.u32();
                    PlayerInput in = r.input();
                    if (!r.bad && frame >= next_broadcast) {
                        pending[slot][frame] = in;
                    }
                }
                break;
            case Msg::Hash:
                if (slot >= 0 && cur_state == State::Racing) {
                    uint32_t frame = r.u32();
                    uint32_t hash = r.u32();
                    if (!r.bad) {
                        hashes[frame][slot] = hash;
                    }
                }
                break;
            default:
                break;
        }
    }

    void client_receive(Reader& r, Msg type) {
        switch (type) {
            case Msg::Welcome: {
                int slot = r.u8();
                std::string name = r.str();
                int max_count = r.u8();
                if (r.bad || slot >= max_players) {
                    return;
                }
                std::fprintf(stderr, "[online] joined lobby \"%s\" as player %d\n", name.c_str(), slot + 1);
                my_slot = slot;
                cur_lobby_name = name;
                max_players_opt = max_count;
                cur_state = State::Lobby;
                error_text.clear();
                break;
            }
            case Msg::Reject:
                error_text = r.str();
                std::fprintf(stderr, "[online] rejected: %s\n", error_text.c_str());
                break;
            case Msg::Roster:
                r.roster(slots);
                break;
            case Msg::Settings: {
                RaceSettings s = r.settings();
                if (!r.bad) {
                    cur_settings = s;
                }
                break;
            }
            case Msg::Start: {
                uint32_t s = r.u32();
                uint32_t d = r.u8();
                RaceSettings settings = r.settings();
                RosterEntry roster[max_players];
                r.roster(roster);
                if (r.bad || cur_state != State::Lobby) {
                    return;
                }
                clear_race();
                seed = s;
                delay = std::max<uint32_t>(d, 1);
                race_set = settings;
                for (int i = 0; i < max_players; i++) {
                    race_slots[i] = roster[i];
                }
                cur_state = State::Loading;
                error_text.clear();
                break;
            }
            case Msg::Go:
                if (cur_state == State::Loading) {
                    go = true;
                    cur_state = State::Racing;
                    inputs_cv.notify_all();
                }
                break;
            case Msg::FrameInputs: {
                uint32_t f = r.u32();
                std::array<PlayerInput, max_players> inputs{};
                for (PlayerInput& in : inputs) {
                    in = r.input();
                }
                if (!r.bad && cur_state == State::Racing) {
                    frames[f] = inputs;
                    inputs_cv.notify_all();
                }
                break;
            }
            case Msg::Desync: {
                uint32_t frame = r.u32();
                end_race("The race went out of sync at frame " + std::to_string(frame) + ". Its log is in the online folder.");
                break;
            }
            case Msg::LoadMismatch:
                end_race("The race couldn't start: the players' games set it up differently.");
                break;
            default:
                break;
        }
    }

    void handle_event(ENetEvent& ev) {
        switch (ev.type) {
            case ENET_EVENT_TYPE_CONNECT:
                enet_peer_timeout(ev.peer, 0, 5000, 15000);
                if (!hosting) {
                    Writer w;
                    w.u8(uint8_t(Msg::Hello));
                    w.u32(protocol_version);
                    w.str(compatibility_key());
                    w.str(my_name);
                    w.car(my_car);
                    w.u8(my_games);
                    send_packet(ev.peer, channel_lobby, w.data);
                }
                break;
            case ENET_EVENT_TYPE_RECEIVE: {
                Reader r(ev.packet->data, ev.packet->dataLength);
                Msg type = Msg(r.u8());
                if (hosting) {
                    host_receive(slot_of(ev.peer), ev.peer, r, type);
                }
                else {
                    client_receive(r, type);
                }
                enet_packet_destroy(ev.packet);
                break;
            }
            case ENET_EVENT_TYPE_DISCONNECT:
                if (hosting) {
                    int slot = slot_of(ev.peer);
                    if (slot >= 0) {
                        slot_peers[slot] = nullptr;
                        if (cur_state == State::Lobby) {
                            slots[slot] = RosterEntry{};
                        }
                        else {
                            // Mid-race the slot keeps its car, which coasts from the next frame not yet broadcast.
                            slots[slot].connected = false;
                            std::fprintf(stderr, "[online] player %d left the race\n", slot + 1);
                        }
                        queue(to_all, channel_lobby, roster_message());
                    }
                }
                else {
                    host_peer = nullptr;
                    if (cur_state == State::Connecting) {
                        if (error_text.empty()) {
                            error_text = "Couldn't reach the host. If they're on another network, they may need to "
                                "forward UDP port " + std::to_string(host_port) + " to their computer.";
                        }
                    }
                    else if (error_text.empty() || cur_state == State::Lobby) {
                        error_text = "The host closed the lobby or the connection was lost.";
                    }
                    reset_session();
                    net_running = false;
                }
                break;
            default:
                break;
        }
    }

    void host_periodic() {
        auto now = clock_type::now();
        if (now - last_beacon >= beacon_period) {
            last_beacon = now;
            send_beacon(beacon_payload(cur_lobby_name, host_port, used_count(), max_players_opt, cur_settings,
                cur_state != State::Lobby));
        }
        if (cur_state == State::Lobby && now - last_roster >= roster_period) {
            last_roster = now;
            bool changed = false;
            for (int i = 1; i < max_players; i++) {
                if (slot_peers[i] != nullptr) {
                    uint16_t ping = uint16_t(std::min<uint32_t>(slot_peers[i]->roundTripTime, 9999));
                    changed |= slots[i].ping_ms != ping;
                    slots[i].ping_ms = ping;
                }
            }
            if (changed) {
                queue(to_all, channel_lobby, roster_message());
            }
        }
        host_check_loaded();
        host_relay();
        host_check_hashes();
    }

    void net_loop() {
        while (net_running) {
            ENetEvent ev;
            int got = enet_host_service(enet_host, &ev, 1);
            std::lock_guard lock{ state_mutex };
            while (got > 0) {
                handle_event(ev);
                got = enet_host_check_events(enet_host, &ev);
            }
            if (hosting) {
                host_periodic();
            }
            else if (cur_state == State::Connecting && clock_type::now() - connect_started > connect_timeout) {
                error_text = "Couldn't reach the host. If they're on another network, they may need to forward UDP "
                    "port " + std::to_string(host_port) + " to their computer.";
                reset_session();
                net_running = false;
            }
            flush_outbox();
            enet_host_flush(enet_host);
        }

        std::lock_guard lock{ state_mutex };
        for (ENetPeer*& p : slot_peers) {
            if (p != nullptr) {
                enet_peer_disconnect_now(p, 0);
                p = nullptr;
            }
        }
        if (host_peer != nullptr) {
            enet_peer_disconnect_now(host_peer, 0);
            host_peer = nullptr;
        }
        enet_host_flush(enet_host);
        enet_host_destroy(enet_host);
        enet_host = nullptr;
        stop_beacons();
    }

    void stop_net_thread() {
        net_running = false;
        if (net_thread.joinable()) {
            net_thread.join();
        }
    }
}

bool rush2::online::available() {
    static std::once_flag once;
    std::call_once(once, []() {
        enet_ready = enet_initialize() == 0;
        if (enet_ready) {
            std::atexit([]() {
                rush2::online::leave();
                enet_deinitialize();
            });
        }
    });
    return enet_ready;
}

std::string rush2::online::clean_name(const std::string& name) {
    std::string out;
    for (char c : name) {
        if (out.size() >= size_t(max_name_length)) {
            break;
        }
        out += (c >= 0x20 && c < 0x7F) ? c : ' ';
    }
    size_t start = out.find_first_not_of(' ');
    size_t end = out.find_last_not_of(' ');
    out = start == std::string::npos ? std::string() : out.substr(start, end - start + 1);
    return out.empty() ? std::string("Player") : out;
}

std::string rush2::online::compatibility_key() {
    return std::string(RUSH2_VERSION) + "-" + RUSH2_BUILD_ID + "-p" + std::to_string(protocol_version);
}

bool rush2::online::host(const HostOptions& options) {
    if (!available()) {
        return false;
    }
    leave();
    ENetAddress address{};
    address.host = ENET_HOST_ANY;
    address.port = options.port;
    ENetHost* h = enet_host_create(&address, max_players - 1, channel_count, 0, 0);
    std::lock_guard lock{ state_mutex };
    if (h == nullptr) {
        error_text = "Couldn't open UDP port " + std::to_string(options.port) + " (is another program or another lobby "
            "using it?).";
        return false;
    }
    enet_host = h;
    host_peer = nullptr;
    for (ENetPeer*& p : slot_peers) {
        p = nullptr;
    }
    hosting = true;
    host_port = options.port;
    cur_lobby_name = options.lobby_name.empty() ? my_name + "'s lobby" : options.lobby_name.substr(0, 32);
    max_players_opt = std::clamp(options.max_players, 1, max_players);
    for (RosterEntry& e : slots) {
        e = RosterEntry{};
    }
    my_slot = 0;
    slots[0].used = true;
    slots[0].connected = true;
    slots[0].ready = true;  // The host starts the race when ready.
    slots[0].name = my_name;
    slots[0].car = my_car;
    slots[0].games = my_games;
    // Test aid (docs/online.md#test-aids): RUSH2_ONLINE_FAKE=K fills K more slots with players who aren't there (their
    // cars get no input, as a dropped player's), to try big races on one machine.
    if (const char* fake = std::getenv("RUSH2_ONLINE_FAKE")) {
        for (int i = 1; i <= std::atoi(fake) && i < max_players; i++) {
            slots[i].used = true;
            slots[i].ready = true;
            slots[i].name = "Fake " + std::to_string(i + 1);
            slots[i].car.type = (uint8_t)i;
        }
    }
    error_text.clear();
    clear_race();
    cur_state = State::Lobby;
    std::fprintf(stderr, "[online] hosting \"%s\" on port %u\n", cur_lobby_name.c_str(), options.port);
    last_beacon = clock_type::time_point{};
    last_roster = clock_type::now();
    net_running = true;
    net_thread = std::thread{ net_loop };
    if (options.open_router_port) {
        upnp_open(options.port);
    }
    return true;
}

bool rush2::online::join(const std::string& text) {
    if (!available()) {
        return false;
    }
    leave();
    std::string name = text;
    uint16_t port = default_port;
    while (!name.empty() && name.back() == ' ') {
        name.pop_back();
    }
    while (!name.empty() && name.front() == ' ') {
        name.erase(name.begin());
    }
    if (std::count(name.begin(), name.end(), ':') > 1 || name.find('[') != std::string::npos) {
        std::lock_guard lock{ state_mutex };
        error_text = "IPv6 addresses aren't supported yet. Use the host's IPv4 address.";
        return false;
    }
    size_t colon = name.find(':');
    if (colon != std::string::npos) {
        int p = std::atoi(name.c_str() + colon + 1);
        name = name.substr(0, colon);
        if (p <= 0 || p > 65535) {
            std::lock_guard lock{ state_mutex };
            error_text = "That port isn't valid.";
            return false;
        }
        port = uint16_t(p);
    }
    ENetAddress address{};
    address.port = port;
    if (name.empty() || enet_address_set_host(&address, name.c_str()) != 0) {
        std::lock_guard lock{ state_mutex };
        error_text = "Couldn't find the address \"" + text + "\".";
        return false;
    }
    ENetHost* h = enet_host_create(nullptr, 1, channel_count, 0, 0);
    if (h == nullptr) {
        std::lock_guard lock{ state_mutex };
        error_text = "Couldn't open a network connection.";
        return false;
    }
    ENetPeer* peer = enet_host_connect(h, &address, channel_count, 0);
    std::lock_guard lock{ state_mutex };
    if (peer == nullptr) {
        enet_host_destroy(h);
        error_text = "Couldn't open a network connection.";
        return false;
    }
    enet_host = h;
    host_peer = peer;
    hosting = false;
    host_port = port;
    my_slot = -1;
    for (RosterEntry& e : slots) {
        e = RosterEntry{};
    }
    error_text.clear();
    clear_race();
    cur_state = State::Connecting;
    connect_started = clock_type::now();
    net_running = true;
    net_thread = std::thread{ net_loop };
    return true;
}

void rush2::online::leave() {
    stop_net_thread();
    upnp_close();
    std::lock_guard lock{ state_mutex };
    std::string kept_error = error_text;
    reset_session();
    error_text = kept_error;
}

State rush2::online::state() {
    std::lock_guard lock{ state_mutex };
    // The net thread ends itself when the connection to the host is lost; the thread object is joined on next use.
    return cur_state;
}

bool rush2::online::is_host() {
    std::lock_guard lock{ state_mutex };
    return hosting && cur_state != State::Offline;
}

int rush2::online::local_slot() {
    std::lock_guard lock{ state_mutex };
    return my_slot;
}

std::vector<RosterEntry> rush2::online::roster() {
    std::lock_guard lock{ state_mutex };
    return std::vector<RosterEntry>(std::begin(slots), std::end(slots));
}

RaceSettings rush2::online::settings() {
    std::lock_guard lock{ state_mutex };
    return cur_settings;
}

int rush2::online::max_players_setting() {
    std::lock_guard lock{ state_mutex };
    return max_players_opt;
}

std::string rush2::online::lobby_name() {
    std::lock_guard lock{ state_mutex };
    return cur_lobby_name;
}

std::string rush2::online::last_error() {
    std::lock_guard lock{ state_mutex };
    return error_text;
}

std::string rush2::online::router_status() {
    return upnp_status();
}

std::vector<std::string> rush2::online::local_addresses() {
    std::vector<std::string> out;
    if (!available()) {
        return out;
    }
    // The address the system would send from to reach the internet: the LAN address other machines use. Connecting a
    // UDP socket sends nothing.
    ENetSocket s = enet_socket_create(ENET_SOCKET_TYPE_DATAGRAM);
    if (s != ENET_SOCKET_NULL) {
        ENetAddress to{};
        enet_address_set_host_ip(&to, "8.8.8.8");
        to.port = 53;
        ENetAddress local{};
        if (enet_socket_connect(s, &to) == 0 && enet_socket_get_address(s, &local) == 0) {
            char ip[64] = {};
            enet_address_get_host_ip(&local, ip, sizeof(ip));
            out.push_back(ip);
        }
        enet_socket_destroy(s);
    }
    return out;
}

std::string rush2::online::public_address() {
    return upnp_public_address();
}

void rush2::online::set_local_games(uint8_t games) {
    std::lock_guard lock{ state_mutex };
    my_games = games;
}

void rush2::online::set_display_name(const std::string& name) {
    std::lock_guard lock{ state_mutex };
    my_name = clean_name(name);
    if (cur_state == State::Offline) {
        return;
    }
    if (hosting) {
        slots[0].name = my_name;
        queue(to_all, channel_lobby, roster_message());
    }
    else {
        Writer w;
        w.u8(uint8_t(Msg::SetName));
        w.str(my_name);
        queue(to_host, channel_lobby, w.data);
    }
}

std::string rush2::online::display_name() {
    std::lock_guard lock{ state_mutex };
    return my_name;
}

void rush2::online::set_car(const CarChoice& car) {
    std::lock_guard lock{ state_mutex };
    my_car = car;
    if (cur_state != State::Lobby) {
        return;
    }
    if (hosting) {
        slots[0].car = car;
        queue(to_all, channel_lobby, roster_message());
    }
    else {
        Writer w;
        w.u8(uint8_t(Msg::SetCar));
        w.car(car);
        queue(to_host, channel_lobby, w.data);
    }
}

void rush2::online::set_ready(bool ready) {
    std::lock_guard lock{ state_mutex };
    if (cur_state != State::Lobby || hosting) {
        return;
    }
    Writer w;
    w.u8(uint8_t(Msg::SetReady));
    w.u8(ready ? 1 : 0);
    queue(to_host, channel_lobby, w.data);
}

void rush2::online::set_settings(const RaceSettings& s) {
    std::lock_guard lock{ state_mutex };
    if (!hosting || cur_state != State::Lobby || s == cur_settings) {
        return;
    }
    cur_settings = s;
    // A change of what's raced makes everyone confirm again.
    for (int i = 1; i < max_players; i++) {
        slots[i].ready = false;
    }
    queue(to_all, channel_lobby, settings_message());
    queue(to_all, channel_lobby, roster_message());
}

bool rush2::online::start_race() {
    std::lock_guard lock{ state_mutex };
    if (!hosting || cur_state != State::Lobby) {
        return false;
    }
    uint32_t max_ping = 0;
    for (int i = 1; i < max_players; i++) {
        if (slots[i].used && !slots[i].ready) {
            error_text = slots[i].name + " isn't ready.";
            return false;
        }
        if (slots[i].used) {
            max_ping = std::max<uint32_t>(max_ping, slots[i].ping_ms);
        }
    }
    clear_race();
    std::random_device rd;
    seed = rd();
    // Test aid (docs/online.md#test-aids): a fixed seed, so separate runs set up the same race.
    if (const char* fixed = std::getenv("RUSH2_ONLINE_SEED")) {
        seed = (uint32_t)std::strtoul(fixed, nullptr, 0);
    }
    // An input travels from its player to the host and on to the others: about a round trip of the slowest player.
    delay = std::clamp<uint32_t>((max_ping + 5 + frame_ms - 1) / frame_ms, 1, max_input_delay);
    race_set = cur_settings;
    for (int i = 0; i < max_players; i++) {
        race_slots[i] = slots[i];
        slots[i].ready = i == 0;
    }
    Writer w;
    w.u8(uint8_t(Msg::Start));
    w.u32(seed);
    w.u8(uint8_t(delay));
    w.settings(race_set);
    w.roster(race_slots);
    queue(to_all, channel_lobby, w.data);
    queue(to_all, channel_lobby, roster_message());
    cur_state = State::Loading;
    error_text.clear();
    return true;
}

bool rush2::online::racing() {
    std::lock_guard lock{ state_mutex };
    return (cur_state == State::Loading || cur_state == State::Racing) && !race_over;
}

void rush2::online::submit_local_input(uint32_t frame, const PlayerInput& input) {
    std::lock_guard lock{ state_mutex };
    if ((cur_state != State::Racing && cur_state != State::Loading) || my_slot < 0) {
        return;
    }
    if (hosting) {
        if (frame >= next_broadcast) {
            pending[my_slot][frame] = input;
        }
    }
    else {
        Writer w;
        w.u8(uint8_t(Msg::Input));
        w.u32(frame);
        w.input(input);
        queue(to_host, channel_inputs, w.data);
    }
}

bool rush2::online::wait_frame_inputs(uint32_t frame, PlayerInput* out) {
    std::unique_lock lock{ state_mutex };
    auto started = clock_type::now();
    while (true) {
        if (race_over || cur_state == State::Offline) {
            waiting = false;
            return false;
        }
        auto it = frames.find(frame);
        if (go && it != frames.end()) {
            std::copy(it->second.begin(), it->second.end(), out);
            frames.erase(frames.begin(), it);
            waiting = false;
            return true;
        }
        if (!waiting) {
            waiting = true;
            waiting_since = started;
        }
        inputs_cv.wait_for(lock, std::chrono::milliseconds(50));
    }
}

uint32_t rush2::online::input_delay() {
    std::lock_guard lock{ state_mutex };
    return delay;
}

uint32_t rush2::online::race_seed() {
    std::lock_guard lock{ state_mutex };
    return seed;
}

RaceSettings rush2::online::race_settings() {
    std::lock_guard lock{ state_mutex };
    return race_set;
}

std::vector<RosterEntry> rush2::online::race_roster() {
    std::lock_guard lock{ state_mutex };
    return std::vector<RosterEntry>(std::begin(race_slots), std::end(race_slots));
}

void rush2::online::report_state_hash(uint32_t frame, uint32_t hash) {
    std::lock_guard lock{ state_mutex };
    if (cur_state != State::Racing || my_slot < 0) {
        return;
    }
    if (hosting) {
        hashes[frame][my_slot] = hash;
    }
    else {
        Writer w;
        w.u8(uint8_t(Msg::Hash));
        w.u32(frame);
        w.u32(hash);
        queue(to_host, channel_lobby, w.data);
    }
}

void rush2::online::report_loaded(uint32_t hash) {
    std::lock_guard lock{ state_mutex };
    if (cur_state != State::Loading || my_slot < 0) {
        return;
    }
    if (hosting) {
        loaded[my_slot] = true;
        loaded_hash[my_slot] = hash;
    }
    else {
        Writer w;
        w.u8(uint8_t(Msg::Loaded));
        w.u32(hash);
        queue(to_host, channel_lobby, w.data);
    }
}

void rush2::online::race_finished() {
    std::lock_guard lock{ state_mutex };
    if (cur_state != State::Loading && cur_state != State::Racing) {
        return;
    }
    clear_race();
    cur_state = State::Lobby;
    if (hosting) {
        // Players who left mid-race leave the lobby now.
        for (int i = 1; i < max_players; i++) {
            if (slots[i].used && !slots[i].connected) {
                slots[i] = RosterEntry{};
            }
        }
        queue(to_all, channel_lobby, roster_message());
    }
}

std::string rush2::online::race_status() {
    std::lock_guard lock{ state_mutex };
    if (cur_state == State::Loading) {
        return "Waiting for every player to load the race...";
    }
    if (!waiting || clock_type::now() - waiting_since < slow_player_notice) {
        return {};
    }
    if (hosting) {
        std::string names;
        for (int i = 0; i < max_players; i++) {
            if (needs_input(i) && i != my_slot && pending[i].find(next_broadcast) == pending[i].end()) {
                names += (names.empty() ? "" : ", ") + slots[i].name;
            }
        }
        if (!names.empty()) {
            return "Waiting for " + names + "...";
        }
    }
    return "Waiting for the other players...";
}

void rush2::online::update() {
    // A client's net thread stops itself when the connection is lost; join it here so the next host or join starts
    // clean.
    if (!net_running && net_thread.joinable()) {
        net_thread.join();
    }
}
