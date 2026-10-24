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
//
// Resyncs: when the machines' races differ (the load check, or a state hash every 30 frames), nothing ends. The host
// marks the next frame it hasn't broadcast (frame_resync): no machine has run it yet. At that frame's start the host's
// game sends its race state (send_snapshot) and every other machine's waits for it and takes it (online_game.cpp),
// so all go on from the same state. Each machine logs what differed (the online folder of its data folder).

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <mutex>
#include <optional>
#include <random>
#include <thread>

#include "enet/enet.h"

#include <cstdarg>
#include <ctime>
#include <filesystem>
#include <string>
#include <vector>

#include "util/file.h"
#include "online_internal.h"
#include "car2049.h"

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
    // A player whose game sends no input for this long while their connection is still up is dropped, so a game that
    // hangs can't hold everyone's race for good. A lost connection is ENet's to find (its timeouts, handle_event).
    constexpr auto slow_player_drop = std::chrono::seconds(30);
    // A player who hasn't loaded the race by then is dropped (a first load can convert a track: give it time).
    constexpr auto load_timeout = std::chrono::seconds(180);
    // ENet gives up on a peer that hasn't acknowledged for timeout_min once its retries are out, and always after
    // timeout_max: generous, so a network hiccup of a few seconds doesn't drop anyone.
    constexpr uint32_t peer_timeout_min_ms = 10000;
    constexpr uint32_t peer_timeout_max_ms = 30000;
    constexpr auto notice_time = std::chrono::seconds(3);         // How long a Notice shows.
    // Consecutive state checks that still differ after resyncs before the race is given up on: the machines set up
    // different races (which the lobby's checks should prevent), and resyncing every second can't hold it.
    constexpr int max_failed_checks = 15;
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
    std::string my_profile_name;   // The save profile picked on Select Player: the lobby shows it if set.
    std::vector<LocalPlayer> my_players = std::vector<LocalPlayer>(1);  // This machine's players (set_cars).
    uint32_t my_track_checked = 0; // The custom track checked for and whether it's missing (set_track_check).
    bool my_lacks_track = false;
    uint8_t my_games = 0;
    std::array<uint32_t, max_mod_cars> my_mod_cars{};
    // Host: how many players each machine (by its first slot) asked for (set_cars), which may be more than fit.
    int wanted_players[max_players] = {};
    std::string my_gameplay;
    uint16_t host_port = default_port;
    // Client: the host's menus (publish_menu).
    MenuState host_menu_state;
    bool have_host_menu = false;
    // Host: the last menu state sent (publish_menu) and how many times since it was the same.
    std::vector<uint8_t> menu_sent;
    int menu_unchanged = 0;

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
    std::map<uint32_t, uint8_t> frame_flags;              // frame_resync
    std::map<uint32_t, std::vector<uint8_t>> snapshots;   // Client: the host's race state for a resync frame.
    clock_type::time_point waiting_since;
    bool waiting = false;
    // A message every machine shows for a while (a resync), and when it came.
    std::string notice_text;
    clock_type::time_point notice_at;
    // Host only.
    std::map<uint32_t, PlayerInput> pending[max_players];
    uint32_t next_broadcast = 0;
    clock_type::time_point last_progress;
    clock_type::time_point load_started;
    bool loaded[max_players];
    uint32_t loaded_hash[max_players];
    uint32_t loaded_track[max_players];   // report_loaded's setup check
    uint32_t loaded_cars[max_players];
    std::map<uint32_t, std::array<std::optional<uint32_t>, max_players>> hashes;
    // Resyncs: the frame to mark (none: UINT32_MAX), the first frame whose hashes count (older ones were taken before
    // the last resync), consecutive checks that differed and the resyncs this race.
    uint32_t resync_frame = UINT32_MAX;
    uint32_t hashes_from = 0;
    int failed_checks = 0;
    int resyncs = 0;

    // Net thread. net_thread is started and joined from the UI thread (host, join, leave) and joined from the game
    // thread (update, after the thread ended itself), so those take thread_mutex: two threads joining the same
    // std::thread at once crashed. update only tries it, so the game thread never waits.
    std::mutex thread_mutex;
    std::thread net_thread;
    std::atomic<bool> net_running = false;
    std::atomic<bool> lobby_lost = false;  // take_lobby_lost
    ENetHost* enet_host = nullptr;
    ENetPeer* host_peer = nullptr;            // Client: the connection to the host.
    ENetPeer* slot_peers[max_players] = {};   // Host: each client's connection.
    clock_type::time_point connect_started;
    clock_type::time_point last_beacon;
    clock_type::time_point last_roster;

    void queue(int target, uint8_t channel, std::vector<uint8_t> data) {
        outbox.push_back({ target, channel, std::move(data) });
    }

    // Client: this machine's progress and the custom track it checked for, to the host.
    void send_progress() {
        Writer w;
        w.u8(uint8_t(Msg::SetProgress));
        w.u8(my_slot >= 0 ? slots[my_slot].progress : 0);
        w.u32(my_track_checked);
        w.u8(my_lacks_track ? 1 : 0);
        queue(to_host, channel_lobby, w.data);
    }

    std::vector<uint8_t> roster_message() {
        RosterEntry entries[max_players];
        for (int i = 0; i < max_players; i++) {
            entries[i] = slots[i];
            entries[i].in_race = cur_state != State::Lobby && race_slots[i].used;
        }
        Writer w;
        w.u8(uint8_t(Msg::Roster));
        w.roster(entries);
        return w.data;
    }

    std::vector<uint8_t> settings_message() {
        Writer w;
        w.u8(uint8_t(Msg::Settings));
        w.settings(cur_settings);
        return w.data;
    }

    // The name the lobby shows for this machine's player.
    std::string shown_name() {
        return my_profile_name.empty() ? my_name : my_profile_name;
    }

    int used_count() {
        int n = 0;
        for (const RosterEntry& e : slots) {
            n += e.used ? 1 : 0;
        }
        return n;
    }

    // Several players on one machine (split screen): the machine's first slot is its connection's, and the players who
    // joined on its other controllers hold more slots (RosterEntry::machine, local_index), which follow the first one's
    // connection, progress and settings.

    // The slot of machine `machine`'s local player `local`, or -1.
    int slot_of_local(int machine, int local) {
        for (int i = 0; i < max_players; i++) {
            if (slots[i].used && slots[i].machine == machine && slots[i].local_index == local) {
                return i;
            }
        }
        return -1;
    }

    // Host: copies what belongs to the machine rather than the player (connection, progress, the custom track check,
    // ROMs, settings and ping) from its first slot to its others.
    void sync_machine(int machine) {
        const RosterEntry& first = slots[machine];
        for (int i = 0; i < max_players; i++) {
            RosterEntry& e = slots[i];
            if (i == machine || !e.used || e.machine != machine) {
                continue;
            }
            e.connected = first.connected;
            e.progress = first.progress;
            e.track_checked = first.track_checked;
            e.lacks_track = first.lacks_track;
            e.games = first.games;
            e.mod_cars = first.mod_cars;
            e.gameplay = first.gameplay;
            e.ping_ms = first.ping_ms;
        }
    }

    // Host: machine `machine` has these players (its car select's): its first slot takes player 1's car, and players 2-4
    // get slots of their own while the lobby has room (start_race refuses a race that left one out).
    void host_set_players(int machine, const std::vector<LocalPlayer>& players) {
        int count = std::clamp<int>((int)players.size(), 1, max_local_players);
        wanted_players[machine] = count;
        RosterEntry& first = slots[machine];
        if (!players.empty()) {
            first.car = players[0].car;
            first.profile = players[0].profile;
            first.side = players[0].side;
            first.mod_spec = players[0].mod_spec;
        }
        for (int local = 1; local < max_local_players; local++) {
            int slot = slot_of_local(machine, local);
            if (local >= count) {
                if (slot >= 0) {
                    slots[slot] = RosterEntry{};
                }
                continue;
            }
            if (slot < 0) {
                for (int i = 0; i < max_players && used_count() < max_players_opt; i++) {
                    if (!slots[i].used) {
                        slot = i;
                        break;
                    }
                }
                if (slot < 0) {
                    continue;
                }
                slots[slot] = RosterEntry{};
                slots[slot].used = true;
                slots[slot].machine = uint8_t(machine);
                slots[slot].local_index = uint8_t(local);
            }
            RosterEntry& e = slots[slot];
            const LocalPlayer& p = players[local];
            e.name = p.name.empty() ? clean_name(first.name.substr(0, max_name_length - 2) + " " + std::to_string(local + 1))
                                    : clean_name(p.name);
            e.car = p.car;
            e.profile = p.profile;
            e.side = p.side;
            e.mod_spec = p.mod_spec;
        }
        sync_machine(machine);
    }

    // The players' names of the connected machines other than `me` whose slots match `pick`, one per machine.
    template <typename Pick>
    std::string machine_names(int me, Pick pick) {
        std::string names;
        for (int i = 0; i < max_players; i++) {
            const RosterEntry& e = slots[i];
            if (e.used && e.connected && e.machine != me && e.local_index == 0 && pick(i)) {
                names += (names.empty() ? "" : ", ") + e.name;
            }
        }
        return names;
    }

    void clear_race() {
        go = false;
        race_over = false;
        frames.clear();
        frame_flags.clear();
        snapshots.clear();
        resync_frame = UINT32_MAX;
        hashes_from = 0;
        failed_checks = 0;
        resyncs = 0;
        notice_text.clear();
        waiting = false;
        for (int i = 0; i < max_players; i++) {
            pending[i].clear();
            loaded[i] = false;
            loaded_hash[i] = 0;
            loaded_track[i] = 0;
            loaded_cars[i] = 0;
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

    // A message for every machine's race overlay for a few seconds (the host also sends it to the others).
    void show_notice(const std::string& text) {
        notice_text = text;
        notice_at = clock_type::now();
    }

    // Host: the races differ. The next frame not yet broadcast starts with the host's race state; the hashes of the
    // frames before it are dropped (they were taken before the fix).
    void schedule_resync(const std::string& why) {
        if (resync_frame != UINT32_MAX) {
            return;  // One is already coming.
        }
        resync_frame = next_broadcast;
        hashes_from = next_broadcast;
        hashes.erase(hashes.begin(), hashes.lower_bound(hashes_from));
        resyncs++;
        rush2::online::session_log("[online] resync %d at frame %u: %s\n", resyncs, resync_frame, why.c_str());
        std::string text = "Resyncing the race with the host...";
        show_notice(text);
        Writer w;
        w.u8(uint8_t(Msg::Notice));
        w.str(text);
        queue(to_all, channel_lobby, w.data);
    }

    void reset_session() {
        have_host_menu = false;
        menu_sent.clear();
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

    // Host, in a race: disconnects machine `machine` (by its first slot); its players' cars coast from the next frame.
    void drop_machine(int machine) {
        if (slot_peers[machine] != nullptr) {
            enet_peer_disconnect_now(slot_peers[machine], 0);
            slot_peers[machine] = nullptr;
        }
        for (RosterEntry& e : slots) {
            if (e.used && e.machine == machine) {
                e.connected = false;
            }
        }
    }

    // Host: broadcasts every frame whose inputs are all in.
    void host_relay() {
        if (!go || race_over || cur_state != State::Racing) {
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
            uint8_t flags = 0;
            if (f == resync_frame) {
                flags |= frame_resync;
                resync_frame = UINT32_MAX;
            }
            Writer w;
            w.u8(uint8_t(Msg::FrameInputs));
            w.u32(f);
            w.u8(flags);
            for (const PlayerInput& in : inputs) {
                w.input(in);
            }
            for (ENetPeer* p : slot_peers) {
                send_packet(p, channel_inputs, w.data);
            }
            frames[f] = inputs;
            frame_flags[f] = flags;
            next_broadcast = f + 1;
            last_progress = clock_type::now();
            inputs_cv.notify_all();
        }

        // A player whose input hasn't come for a long time is dropped (their car coasts from the next frame).
        if (clock_type::now() - last_progress > slow_player_drop) {
            for (int i = 0; i < max_players; i++) {
                int machine = slots[i].machine;
                if (machine != my_slot && needs_input(i) && pending[i].find(next_broadcast) == pending[i].end() &&
                    slot_peers[machine] != nullptr) {
                    drop_machine(machine);
                    rush2::online::session_log("[online] dropped player %d (no input for frame %u)\n", i + 1, next_broadcast);
                }
            }
            last_progress = clock_type::now();
        }
    }

    // Host: every machine reported loaded: compares their starting states and lets the race begin.
    void host_check_loaded() {
        if (cur_state != State::Loading || go || race_over) {
            return;
        }
        // A player still not loaded long after the race started is dropped, as a slow player in the race is (their
        // car coasts); this machine's own load is never given up on.
        if (clock_type::now() - load_started > load_timeout) {
            for (int i = 0; i < max_players; i++) {
                int machine = slots[i].machine;
                if (machine != my_slot && needs_input(i) && !loaded[i] && slot_peers[machine] != nullptr) {
                    drop_machine(machine);
                    rush2::online::session_log("[online] dropped player %d (didn't load the race)\n", i + 1);
                    queue(to_all, channel_lobby, roster_message());
                }
            }
        }
        // What each machine set up: the same track, or the race can't be run (a resync copies the race's state, not the
        // track). The host's is the reference: it names who differs. The cars never stop a race: a resync copies their
        // physics data too (rush2::car2049::physics_online_sync), and the differing start hash below resyncs frame 0,
        // so a car set up differently on a machine takes the host's; the cars' fingerprint is only logged.
        int reference_slot = -1;
        for (int i = 0; i < max_players; i++) {
            if (!needs_input(i)) {
                continue;
            }
            if (!loaded[i]) {
                return;
            }
            bool host_slot = race_slots[i].machine == my_slot;
            if (reference_slot < 0 || (host_slot && race_slots[reference_slot].machine != my_slot)) {
                reference_slot = i;
            }
        }
        for (int i = 0; i < max_players && reference_slot >= 0; i++) {
            if (!needs_input(i) || race_slots[i].machine == race_slots[reference_slot].machine) {
                continue;
            }
            if (loaded_cars[i] != loaded_cars[reference_slot]) {
                rush2::online::session_log("[online] player %d set the cars up differently (cars %08X, host %08X): "
                    "they take the host's at the first resync\n", i + 1, loaded_cars[i], loaded_cars[reference_slot]);
            }
            std::string what;
            if (loaded_track[i] != loaded_track[reference_slot]) {
                what = "loaded a different track than " + slots[reference_slot].name + "'s (for a custom track: other "
                    "files of it; for another game's track: another copy of that game's ROM). Everyone needs the same "
                    "track files.";
            }
            if (!what.empty()) {
                std::string reason = "The race couldn't start: " + slots[i].name + "'s game " + what;
                rush2::online::session_log("[online] %s (track %08X/%08X, cars %08X/%08X)\n", reason.c_str(), loaded_track[i],
                    loaded_track[reference_slot], loaded_cars[i], loaded_cars[reference_slot]);
                Writer w;
                w.u8(uint8_t(Msg::LoadMismatch));
                w.str(reason);
                for (ENetPeer* p : slot_peers) {
                    send_packet(p, channel_lobby, w.data);
                }
                end_race(reason);
                return;
            }
        }
        std::optional<uint32_t> reference;
        for (int i = 0; i < max_players; i++) {
            if (!needs_input(i)) {
                continue;
            }
            if (!reference) {
                reference = loaded_hash[i];
            }
            else if (*reference != loaded_hash[i]) {
                // The race's first frame (the frame not yet broadcast is frame 0) starts from the host's state on
                // every machine.
                char why[96];
                std::snprintf(why, sizeof(why), "player %d loaded a different start (%08X, expected %08X)", i + 1,
                    loaded_hash[i], *reference);
                schedule_resync(why);
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
        if (race_over) {
            hashes.clear();
            return;
        }
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
                if (++failed_checks >= max_failed_checks) {
                    rush2::online::session_log("[online] %s's race can't be held in step (frame %u)\n",
                        slots[differs].name.c_str(), frame);
                    // Every machine ends the race (Msg::LoadMismatch ends it with the reason): with only the host's
                    // ended, the others waited for the next frame's inputs for ever.
                    std::string reason = "The race couldn't be kept in step with " + slots[differs].name + "'s game, "
                        "which seems to have set it up differently. What differed is in the online folder of each "
                        "game's data folder.";
                    Writer w;
                    w.u8(uint8_t(Msg::LoadMismatch));
                    w.str(reason);
                    for (ENetPeer* p : slot_peers) {
                        send_packet(p, channel_lobby, w.data);
                    }
                    end_race(reason);
                    hashes.clear();
                    return;
                }
                schedule_resync("frame " + std::to_string(frame) + ": player " + std::to_string(differs + 1) +
                    "'s state differs");
                // schedule_resync dropped this frame's hashes and every earlier frame's.
                it = hashes.lower_bound(hashes_from);
                continue;
            }
            failed_checks = 0;
            it = hashes.erase(it);
        }
    }

    void host_receive(int slot, ENetPeer* peer, Reader& r, Msg type) {
        switch (type) {
            case Msg::Hello: {
                auto reject = [&](const std::string& reason) {
                    Writer w;
                    w.u8(uint8_t(Msg::Reject));
                    w.str(reason);
                    send_packet(peer, channel_lobby, w.data);
                    enet_peer_disconnect_later(peer, 0);
                };
                // The version first: another version's hello may be laid out differently (reading all of it first
                // ignored such a hello, and its player saw only "couldn't reach the host").
                uint32_t protocol = r.u32();
                std::string key = r.str();
                if (protocol != protocol_version || key != compatibility_key()) {
                    reject("Your game is a different version than the host's (host: " + compatibility_key() +
                        ", yours: " + (r.bad ? std::string("older") : key) + "). Both need the same release.");
                    return;
                }
                std::string name = clean_name(r.str());
                CarChoice car = r.car();
                uint8_t games = r.u8();
                std::string gameplay = r.long_str();
                std::array<uint32_t, max_mod_cars> mod_cars = r.mod_cars();
                if (r.bad) {
                    reject("The host couldn't read your game's hello.");
                    return;
                }
                // A player may join while a race is under way: they wait in the lobby for the next one (their slot isn't
                // in the race's roster, so the race neither waits for nor checks them).
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
                rush2::online::session_log("[online] %s joined as player %d\n", name.c_str(), free + 1);
                slot_peers[free] = peer;
                enet_peer_timeout(peer, 0, peer_timeout_min_ms, peer_timeout_max_ms);
                RosterEntry& e = slots[free];
                e = RosterEntry{};
                e.used = true;
                e.connected = true;
                e.machine = uint8_t(free);
                e.name = name;
                e.car = car;
                e.games = games;
                e.mod_cars = mod_cars;
                e.gameplay = gameplay;
                wanted_players[free] = 1;
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
            case Msg::SetCars:
                if (slot >= 0) {
                    int count = r.u8();
                    std::vector<LocalPlayer> players;
                    for (int i = 0; i < count && i < max_local_players && !r.bad; i++) {
                        LocalPlayer p;
                        p.name = r.str();
                        p.car = r.car();
                        p.profile = r.bytes();
                        p.side = r.bytes();
                        p.mod_spec = r.bytes();
                        players.push_back(std::move(p));
                    }
                    if (!r.bad && !players.empty() && cur_state == State::Lobby) {
                        host_set_players(slot, players);
                        queue(to_all, channel_lobby, roster_message());
                    }
                }
                break;
            case Msg::SetProgress:
                if (slot >= 0) {
                    uint8_t progress = r.u8();
                    uint32_t checked = r.u32();
                    bool lacks = r.u8() != 0;
                    if (!r.bad && cur_state == State::Lobby && (progress != slots[slot].progress ||
                        checked != slots[slot].track_checked || lacks != slots[slot].lacks_track)) {
                        slots[slot].progress = progress;
                        slots[slot].track_checked = checked;
                        slots[slot].lacks_track = lacks;
                        sync_machine(slot);
                        queue(to_all, channel_lobby, roster_message());
                    }
                }
                break;
            case Msg::SetGameplay:
                if (slot >= 0) {
                    std::string gameplay = r.long_str();
                    if (!r.bad && cur_state == State::Lobby) {
                        slots[slot].gameplay = gameplay;
                        sync_machine(slot);
                        queue(to_all, channel_lobby, roster_message());
                    }
                }
                break;
            case Msg::SetModCars:
                if (slot >= 0) {
                    std::array<uint32_t, max_mod_cars> mod_cars = r.mod_cars();
                    if (!r.bad && mod_cars != slots[slot].mod_cars) {
                        slots[slot].mod_cars = mod_cars;
                        sync_machine(slot);
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
                    uint32_t track = r.u32();
                    uint32_t cars = r.u32();
                    if (!r.bad) {
                        for (int i = 0; i < max_players; i++) {
                            if (race_slots[i].used && race_slots[i].machine == slot) {
                                loaded[i] = true;
                                loaded_hash[i] = hash;
                                loaded_track[i] = track;
                                loaded_cars[i] = cars;
                            }
                        }
                        host_check_loaded();
                    }
                }
                break;
            case Msg::Input:
                // A machine sends its first inputs while the others may still be loading.
                if (slot >= 0 && (cur_state == State::Racing || cur_state == State::Loading)) {
                    uint32_t frame = r.u32();
                    int count = r.u8();
                    PlayerInput inputs[max_local_players];
                    for (int i = 0; i < count && i < max_local_players; i++) {
                        inputs[i] = r.input();
                    }
                    if (!r.bad && frame >= next_broadcast) {
                        for (int i = 0; i < max_players; i++) {
                            const RosterEntry& e = race_slots[i];
                            if (e.used && e.machine == slot && e.local_index < count) {
                                pending[i][frame] = inputs[e.local_index];
                            }
                        }
                    }
                }
                break;
            case Msg::Hash:
                if (slot >= 0 && cur_state == State::Racing) {
                    uint32_t frame = r.u32();
                    uint32_t hash = r.u32();
                    if (!r.bad && frame >= hashes_from) {
                        for (int i = 0; i < max_players; i++) {
                            if (race_slots[i].used && race_slots[i].machine == slot) {
                                hashes[frame][i] = hash;
                            }
                        }
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
                rush2::online::session_log("[online] joined lobby \"%s\" as player %d\n", name.c_str(), slot + 1);
                my_slot = slot;
                my_track_checked = 0;
                my_lacks_track = false;
                cur_lobby_name = name;
                max_players_opt = max_count;
                cur_state = State::Lobby;
                error_text.clear();
                break;
            }
            case Msg::Reject:
                error_text = r.str();
                rush2::online::session_log("[online] rejected: %s\n", error_text.c_str());
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
                for (RosterEntry& e : roster) {
                    e.profile = r.bytes();
                    e.side = r.bytes();
                    e.mod_spec = r.bytes();
                }
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
            case Msg::MenuState: {
                MenuState m;
                m.phase = MenuPhase(r.u8());
                m.settings = r.settings();
                m.option_cursor = int8_t(r.u8());
                m.option_top = int8_t(r.u8());
                m.start_error = r.str();
                m.offered_tracks = r.bytes();
                if (r.bad) {
                    return;
                }
                host_menu_state = m;
                have_host_menu = true;
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
                uint8_t flags = r.u8();
                std::array<PlayerInput, max_players> inputs{};
                for (PlayerInput& in : inputs) {
                    in = r.input();
                }
                // Also while still loading: Go travels on another channel, and after a lost packet the first frames can
                // come before it (dropping them left this machine waiting on frame 0 until the host dropped it).
                if (!r.bad && (cur_state == State::Racing || cur_state == State::Loading)) {
                    frames[f] = inputs;
                    frame_flags[f] = flags;
                    inputs_cv.notify_all();
                }
                break;
            }
            case Msg::Snapshot: {
                uint32_t f = r.u32();
                std::vector<uint8_t> state = r.bytes();
                if (!r.bad && (cur_state == State::Racing || cur_state == State::Loading)) {
                    snapshots[f] = std::move(state);
                    inputs_cv.notify_all();
                }
                break;
            }
            case Msg::Notice: {
                std::string text = r.str();
                if (!r.bad) {
                    show_notice(text);
                }
                break;
            }
            case Msg::LoadMismatch: {
                std::string reason = r.str();
                end_race(r.bad || reason.empty() ? std::string("The race couldn't start: the games set it up differently.")
                                                 : reason);
                break;
            }
            default:
                break;
        }
    }

    void handle_event(ENetEvent& ev) {
        switch (ev.type) {
            case ENET_EVENT_TYPE_CONNECT:
                enet_peer_timeout(ev.peer, 0, peer_timeout_min_ms, peer_timeout_max_ms);
                if (!hosting) {
                    Writer w;
                    w.u8(uint8_t(Msg::Hello));
                    w.u32(protocol_version);
                    w.str(compatibility_key());
                    w.str(shown_name());
                    w.car(my_players[0].car);
                    w.u8(my_games);
                    w.long_str(my_gameplay);
                    w.mod_cars(my_mod_cars);
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
                        // The machine's every player goes with its connection.
                        for (RosterEntry& e : slots) {
                            if (!e.used || e.machine != slot) {
                                continue;
                            }
                            if (cur_state == State::Lobby) {
                                e = RosterEntry{};
                            }
                            else {
                                // Mid-race the slot keeps its car, which coasts from the next frame not yet broadcast.
                                e.connected = false;
                            }
                        }
                        wanted_players[slot] = 0;
                        if (cur_state != State::Lobby) {
                            rush2::online::session_log("[online] player %d left the race\n", slot + 1);
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
                    else {
                        if (error_text.empty() || cur_state == State::Lobby) {
                            error_text = "The host closed the lobby or the connection was lost.";
                        }
                        lobby_lost = true;
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
                    sync_machine(i);
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
        stop_beacons(beacon_payload(cur_lobby_name, host_port, 0, 0, cur_settings, false));
    }

    void stop_net_thread() {
        std::lock_guard lock{ thread_mutex };
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
            // Registered after the threads' static std::thread objects, so it runs before their destructors: a
            // still-joinable one would abort() the exit (Windows then writes a crash dump, a slow close).
            std::atexit([]() {
                rush2::online::leave();
                rush2::online::stop_lan_listener();
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
    slots[0].name = shown_name();
    slots[0].games = my_games;
    slots[0].mod_cars = my_mod_cars;
    slots[0].gameplay = my_gameplay;
    for (int& w : wanted_players) {
        w = 0;
    }
    host_set_players(0, my_players);
    // Test aid (docs/online.md#test-aids): RUSH2_ONLINE_FAKE=K fills K more slots with players who aren't there (their
    // cars get no input, as a dropped player's), to try big races on one machine.
    if (const char* fake = std::getenv("RUSH2_ONLINE_FAKE")) {
        for (int i = 1; i <= std::atoi(fake) && i < max_players; i++) {
            if (slots[i].used) {
                continue;
            }
            slots[i].used = true;
            slots[i].machine = uint8_t(i);
            slots[i].progress = uint8_t(Progress::CarChosen);
            slots[i].name = "Fake " + std::to_string(i + 1);
            slots[i].car.type = (uint8_t)i;
            slots[i].gameplay = my_gameplay;
        }
    }
    error_text.clear();
    clear_race();
    cur_state = State::Lobby;
    rush2::online::session_log("[online] hosting \"%s\" on port %u\n", cur_lobby_name.c_str(), options.port);
    last_beacon = clock_type::time_point{};
    last_roster = clock_type::now();
    {
        std::lock_guard thread_lock{ thread_mutex };
        net_running = true;
        net_thread = std::thread{ net_loop };
    }
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
    lobby_lost = false;
    clear_race();
    cur_state = State::Connecting;
    connect_started = clock_type::now();
    {
        std::lock_guard thread_lock{ thread_mutex };
        net_running = true;
        net_thread = std::thread{ net_loop };
    }
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
    std::vector<RosterEntry> out(std::begin(slots), std::end(slots));
    if (hosting) {
        for (int i = 0; i < max_players; i++) {
            out[i].in_race = cur_state != State::Lobby && race_slots[i].used;
        }
    }
    return out;
}

int rush2::online::max_players_setting() {
    std::lock_guard lock{ state_mutex };
    return max_players_opt;
}

std::string rush2::online::lobby_name() {
    std::lock_guard lock{ state_mutex };
    return cur_lobby_name;
}

uint16_t rush2::online::lobby_port() {
    std::lock_guard lock{ state_mutex };
    return host_port;
}

std::string rush2::online::last_error() {
    std::lock_guard lock{ state_mutex };
    return error_text;
}

bool rush2::online::take_lobby_lost() {
    return lobby_lost.exchange(false);
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

void rush2::online::set_local_mod_cars(const std::array<uint32_t, max_mod_cars>& checks) {
    std::lock_guard lock{ state_mutex };
    if (checks == my_mod_cars) {
        return;
    }
    my_mod_cars = checks;
    if (cur_state == State::Offline || cur_state == State::Connecting || my_slot < 0) {
        return;  // The hello carries them.
    }
    if (hosting) {
        slots[0].mod_cars = my_mod_cars;
        sync_machine(0);
        queue(to_all, channel_lobby, roster_message());
    }
    else {
        Writer w;
        w.u8(uint8_t(Msg::SetModCars));
        w.mod_cars(my_mod_cars);
        queue(to_host, channel_lobby, w.data);
    }
}

void rush2::online::set_local_gameplay(const std::string& gameplay) {
    std::lock_guard lock{ state_mutex };
    if (gameplay == my_gameplay) {
        return;
    }
    my_gameplay = gameplay;
    if (cur_state != State::Lobby) {
        return;
    }
    if (hosting) {
        slots[0].gameplay = my_gameplay;
        queue(to_all, channel_lobby, roster_message());
    }
    else {
        Writer w;
        w.u8(uint8_t(Msg::SetGameplay));
        w.long_str(my_gameplay);
        queue(to_host, channel_lobby, w.data);
    }
}

namespace {
    // Sends this machine's shown name to the lobby (state_mutex held).
    void send_name() {
        if (cur_state == State::Offline || cur_state == State::Connecting) {
            return;
        }
        if (hosting) {
            if (slots[0].name != shown_name()) {
                slots[0].name = shown_name();
                queue(to_all, channel_lobby, roster_message());
            }
        }
        else {
            Writer w;
            w.u8(uint8_t(Msg::SetName));
            w.str(shown_name());
            queue(to_host, channel_lobby, w.data);
        }
    }
}

void rush2::online::set_display_name(const std::string& name) {
    std::lock_guard lock{ state_mutex };
    std::string before = shown_name();
    my_name = clean_name(name);
    if (shown_name() != before) {
        send_name();
    }
}

void rush2::online::set_profile_name(const std::string& name) {
    std::lock_guard lock{ state_mutex };
    std::string cleaned = name.empty() ? std::string() : clean_name(name);
    if (cleaned == my_profile_name) {
        return;
    }
    my_profile_name = cleaned;
    send_name();
}

std::string rush2::online::display_name() {
    std::lock_guard lock{ state_mutex };
    return my_name;
}

void rush2::online::set_cars(const std::vector<LocalPlayer>& players) {
    std::lock_guard lock{ state_mutex };
    if (players.empty()) {
        return;
    }
    my_players.assign(players.begin(), players.begin() + std::min<size_t>(players.size(), max_local_players));
    if (cur_state != State::Lobby) {
        return;
    }
    if (hosting) {
        host_set_players(0, my_players);
        queue(to_all, channel_lobby, roster_message());
    }
    else {
        Writer w;
        w.u8(uint8_t(Msg::SetCars));
        w.u8(uint8_t(my_players.size()));
        for (const LocalPlayer& p : my_players) {
            w.str(p.name);
            w.car(p.car);
            w.bytes(p.profile);
            w.bytes(p.side);
            w.bytes(p.mod_spec);
        }
        queue(to_host, channel_lobby, w.data);
    }
}

void rush2::online::set_progress(Progress progress) {
    std::lock_guard lock{ state_mutex };
    if (cur_state != State::Lobby) {
        return;
    }
    if (hosting) {
        if (slots[0].progress != uint8_t(progress)) {
            slots[0].progress = uint8_t(progress);
            sync_machine(0);
            queue(to_all, channel_lobby, roster_message());
        }
        return;
    }
    if (my_slot < 0 || slots[my_slot].progress == uint8_t(progress)) {
        return;
    }
    slots[my_slot].progress = uint8_t(progress);  // Until the host's roster says so.
    send_progress();
}

void rush2::online::set_track_check(uint32_t checked, bool lacks) {
    std::lock_guard lock{ state_mutex };
    if (cur_state != State::Lobby || hosting || my_slot < 0 ||
        (my_track_checked == checked && my_lacks_track == lacks)) {
        return;
    }
    my_track_checked = checked;
    my_lacks_track = lacks;
    send_progress();
}

uint32_t rush2::online::track_check(const std::string& mod_track) {
    if (mod_track.empty()) {
        return 0;
    }
    uint32_t h = 2166136261u;
    for (char c : mod_track) {
        h = (h ^ uint8_t(c)) * 16777619u;
    }
    return h == 0 ? 1 : h;
}

void rush2::online::publish_menu(const MenuState& m) {
    std::lock_guard lock{ state_mutex };
    if (!hosting || cur_state != State::Lobby) {
        return;
    }
    cur_settings = m.settings;
    Writer w;
    w.u8(uint8_t(Msg::MenuState));
    w.u8(uint8_t(m.phase));
    w.settings(m.settings);
    w.u8(uint8_t(m.option_cursor));
    w.u8(uint8_t(m.option_top));
    w.str(m.start_error);
    w.bytes(m.offered_tracks);
    // Sent reliably: a change at once, the same state again twice a second (for players who just joined).
    if (w.data == menu_sent && ++menu_unchanged < 30) {
        return;
    }
    menu_unchanged = 0;
    menu_sent = w.data;
    queue(to_all, channel_lobby, w.data);
}

bool rush2::online::host_menu(MenuState* out) {
    std::lock_guard lock{ state_mutex };
    if (hosting || !have_host_menu) {
        return false;
    }
    *out = host_menu_state;
    return true;
}

bool rush2::online::start_race(const RaceSettings& settings) {
    std::lock_guard lock{ state_mutex };
    if (!hosting || cur_state != State::Lobby) {
        return false;
    }
    cur_settings = settings;
    error_text.clear();
    uint32_t max_ping = 0;
    for (int i = 1; i < max_players; i++) {
        if (slots[i].used && slots[i].connected && slots[i].progress != uint8_t(Progress::CarChosen)) {
            error_text = slots[i].name + " hasn't picked a car.";
            return false;
        }
        if (slots[i].used && slots[i].connected && (slots[i].games & cur_settings.needs_games) != cur_settings.needs_games) {
            error_text = slots[i].name + " doesn't have a game ROM this race needs (the Games tab).";
            return false;
        }
        if (slots[i].used && slots[i].connected && slots[i].gameplay != slots[0].gameplay) {
            // Their game takes these settings once no menu is open on it (rush2::online_tab::follow_host_settings).
            error_text = "Waiting for " + slots[i].name + "'s game to take your settings (a menu is open on it).";
            return false;
        }
        if (slots[i].used) {
            max_ping = std::max<uint32_t>(max_ping, slots[i].ping_ms);
        }
    }
    // Every machine's players have a slot: one that brought more players than the lobby had room for waits.
    for (int m = 0; m < max_players; m++) {
        if (!slots[m].used || !slots[m].connected || slots[m].machine != m) {
            continue;
        }
        int have = 0;
        for (const RosterEntry& e : slots) {
            have += e.used && e.machine == m ? 1 : 0;
        }
        if (have < wanted_players[m]) {
            error_text = "The lobby is full: " + std::to_string(have) + " of " + slots[m].name + "'s " +
                std::to_string(wanted_players[m]) + " players fit (Max Players is " + std::to_string(max_players_opt) +
                "). A player there has to leave Select Player for the race to start.";
            return false;
        }
    }
    // Every car is picked by now. A Rush 2049 car (types 23-35, src/rush2049/car2049.cpp) loads from the Rush 2049 ROM
    // on every machine, a California Speed car (36-56, src/calspeed/calspeed_cars.cpp) from its ROM (the cars options
    // already match, with the rest of the gameplay settings). A mod's car (57-72, src/car_mods.cpp) races on every
    // machine, whatever mods it has: one without that car shows a stand-in with the car's physics
    // (docs/online.md#mod-cars).
    for (bool calspeed : { false, true }) {
        std::string driver;
        for (const RosterEntry& e : slots) {
            if (e.used && e.connected && e.car.type >= rush2::car2049::first_type &&
                !rush2::car_types::is_mod(e.car.type) && rush2::car_types::is_calspeed(e.car.type) == calspeed) {
                driver = e.name;
                break;
            }
        }
        uint8_t game = calspeed ? games_calspeed : games_rush2049;
        for (int i = 0; i < max_players && !driver.empty(); i++) {
            if (slots[i].used && slots[i].connected && (slots[i].games & game) == 0) {
                error_text = slots[i].name + " doesn't have the " + (calspeed ? "California Speed" : "Rush 2049") +
                    " ROM that " + driver + "'s car needs (the Games tab).";
                return false;
            }
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
    }
    Writer w;
    w.u8(uint8_t(Msg::Start));
    w.u32(seed);
    w.u8(uint8_t(delay));
    w.settings(race_set);
    w.roster(race_slots);
    for (const RosterEntry& e : race_slots) {
        w.bytes(e.profile);
        w.bytes(e.side);
        w.bytes(e.mod_spec);
    }
    queue(to_all, channel_lobby, w.data);
    queue(to_all, channel_lobby, roster_message());
    cur_state = State::Loading;
    load_started = clock_type::now();
    error_text.clear();
    return true;
}

bool rush2::online::racing() {
    std::lock_guard lock{ state_mutex };
    return (cur_state == State::Loading || cur_state == State::Racing) && !race_over;
}

void rush2::online::submit_local_input(uint32_t frame, const std::vector<PlayerInput>& inputs) {
    std::lock_guard lock{ state_mutex };
    if ((cur_state != State::Racing && cur_state != State::Loading) || my_slot < 0 || inputs.empty()) {
        return;
    }
    int count = std::min<int>((int)inputs.size(), max_local_players);
    if (hosting) {
        if (frame >= next_broadcast) {
            for (int i = 0; i < max_players; i++) {
                const RosterEntry& e = race_slots[i];
                if (e.used && e.machine == my_slot && e.local_index < count) {
                    pending[i][frame] = inputs[e.local_index];
                }
            }
        }
    }
    else {
        Writer w;
        w.u8(uint8_t(Msg::Input));
        w.u32(frame);
        w.u8(uint8_t(count));
        for (int i = 0; i < count; i++) {
            w.input(inputs[i]);
        }
        queue(to_host, channel_inputs, w.data);
    }
}

rush2::online::Wait rush2::online::wait_frame_inputs(uint32_t frame, PlayerInput* out, int timeout_ms, bool* resync) {
    std::unique_lock lock{ state_mutex };
    auto started = clock_type::now();
    while (true) {
        if (race_over || cur_state == State::Offline) {
            waiting = false;
            return Wait::Ended;
        }
        auto it = frames.find(frame);
        if (go && it != frames.end()) {
            std::copy(it->second.begin(), it->second.end(), out);
            if (resync != nullptr) {
                auto flags = frame_flags.find(frame);
                *resync = flags != frame_flags.end() && (flags->second & frame_resync) != 0;
            }
            frames.erase(frames.begin(), it);
            frame_flags.erase(frame_flags.begin(), frame_flags.lower_bound(frame));
            waiting = false;
            return Wait::Ready;
        }
        if (!waiting) {
            waiting = true;
            waiting_since = started;
        }
        if (timeout_ms >= 0 && clock_type::now() - started >= std::chrono::milliseconds(timeout_ms)) {
            return Wait::Timeout;
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
        if (frame < hashes_from) {
            return;  // Taken before the last resync.
        }
        for (int i = 0; i < max_players; i++) {
            if (race_slots[i].used && race_slots[i].machine == my_slot) {
                hashes[frame][i] = hash;
            }
        }
    }
    else {
        Writer w;
        w.u8(uint8_t(Msg::Hash));
        w.u32(frame);
        w.u32(hash);
        queue(to_host, channel_lobby, w.data);
    }
}

void rush2::online::report_loaded(uint32_t hash, uint32_t track, uint32_t cars) {
    std::lock_guard lock{ state_mutex };
    if (cur_state != State::Loading || my_slot < 0) {
        return;
    }
    if (hosting) {
        for (int i = 0; i < max_players; i++) {
            if (race_slots[i].used && race_slots[i].machine == my_slot) {
                loaded[i] = true;
                loaded_hash[i] = hash;
                loaded_track[i] = track;
                loaded_cars[i] = cars;
            }
        }
    }
    else {
        Writer w;
        w.u8(uint8_t(Msg::Loaded));
        w.u32(hash);
        w.u32(track);
        w.u32(cars);
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
    // Everyone goes through the menus again (their progress comes back as their games get there).
    for (RosterEntry& e : slots) {
        e.progress = uint8_t(Progress::Lobby);
    }
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
    auto now = clock_type::now();
    if (cur_state == State::Loading) {
        if (hosting) {
            std::string names = machine_names(my_slot, [](int i) { return needs_input(i) && !loaded[i]; });
            if (!names.empty()) {
                return "Waiting for " + names + " to load the race...";
            }
        }
        return "Waiting for every player to load the race...";
    }
    bool notice = !notice_text.empty() && now - notice_at < notice_time;
    if (!waiting || now - waiting_since < slow_player_notice) {
        return notice ? notice_text : std::string();
    }
    if (hosting) {
        std::string names = machine_names(my_slot, [](int i) {
            // A machine's input comes in one message: its first player stands for it.
            return needs_input(i) && pending[i].find(next_broadcast) == pending[i].end();
        });
        if (!names.empty()) {
            auto left = std::chrono::duration_cast<std::chrono::seconds>(slow_player_drop - (now - last_progress));
            return "Waiting for " + names + "... (they leave the race in " +
                std::to_string(std::max<long long>(left.count(), 0)) + " s if their game doesn't answer)";
        }
    }
    return notice ? notice_text : std::string("Waiting for the other players...");
}

void rush2::online::send_snapshot(uint32_t frame, const std::vector<uint8_t>& state) {
    std::lock_guard lock{ state_mutex };
    if (!hosting || cur_state != State::Racing) {
        return;
    }
    Writer w;
    w.u8(uint8_t(Msg::Snapshot));
    w.u32(frame);
    w.bytes(state);
    queue(to_all, channel_inputs, w.data);
}

rush2::online::Wait rush2::online::wait_snapshot(uint32_t frame, std::vector<uint8_t>* out, int timeout_ms) {
    std::unique_lock lock{ state_mutex };
    auto started = clock_type::now();
    while (true) {
        if (race_over || cur_state == State::Offline) {
            return Wait::Ended;
        }
        auto it = snapshots.find(frame);
        if (it != snapshots.end()) {
            *out = std::move(it->second);
            snapshots.erase(snapshots.begin(), std::next(it));
            return Wait::Ready;
        }
        if (clock_type::now() - started >= std::chrono::milliseconds(timeout_ms)) {
            return Wait::Timeout;
        }
        inputs_cv.wait_for(lock, std::chrono::milliseconds(20));
    }
}

void rush2::online::update() {
    // A client's net thread stops itself when the connection is lost; join it here so the next host or join starts
    // clean.
    std::unique_lock lock{ thread_mutex, std::try_to_lock };
    if (lock.owns_lock() && !net_running && net_thread.joinable()) {
        net_thread.join();
    }
}

void rush2::online::session_log(const char* format, ...) {
    static std::mutex mutex;
    static FILE* file = nullptr;
    static bool opened = false;
    char text[2048];
    va_list args;
    va_start(args, format);
    std::vsnprintf(text, sizeof(text), format, args);
    va_end(args);
    std::fputs(text, stderr);
    std::lock_guard lock{ mutex };
    if (!opened) {
        opened = true;
        std::error_code ec;
        std::filesystem::path dir = recompui::file::get_app_folder_path() / "online";
        std::filesystem::create_directories(dir, ec);
        // The oldest sessions' logs go, so the folder doesn't grow without end.
        std::vector<std::filesystem::path> old;
        for (const auto& e : std::filesystem::directory_iterator(dir, ec)) {
            std::string name = e.path().filename().string();
            if (name.rfind("session_", 0) == 0 && e.path().extension() == ".txt") {
                old.push_back(e.path());
            }
        }
        std::sort(old.begin(), old.end());
        for (size_t i = 0; i + 19 < old.size(); i++) {
            std::filesystem::remove(old[i], ec);
        }
        std::time_t now = std::time(nullptr);
        char name[64];
        std::strftime(name, sizeof(name), "session_%Y%m%d_%H%M%S.txt", std::localtime(&now));
        file = std::fopen((dir / name).string().c_str(), "a");
    }
    if (file != nullptr) {
        std::time_t now = std::time(nullptr);
        char stamp[16];
        std::strftime(stamp, sizeof(stamp), "%H:%M:%S ", std::localtime(&now));
        std::fputs(stamp, file);
        std::fputs(text, file);
        std::fflush(file);
    }
}
