// Online play: the LAN lobby list (docs/online.md#lan-lobbies). A host sends a beacon about once a second to the LAN's
// broadcast address and to this computer (so other copies of the game on the same computer hear it too) on UDP port
// rush2::online::discovery_port; the Online tab's list is every beacon heard in the last few seconds. Beacons only
// advertise: joining connects to the address the beacon came from.

#include <atomic>
#include <chrono>
#include <map>
#include <mutex>
#include <thread>

#include "enet/enet.h"

#include "online_internal.h"

namespace {
    using clock_type = std::chrono::steady_clock;
    constexpr char magic[4] = { 'R', '2', 'O', 'L' };
    constexpr auto lobby_lifetime = std::chrono::seconds(4);

    ENetSocket send_socket = ENET_SOCKET_NULL;

    std::mutex listen_mutex;
    std::thread listen_thread;
    std::atomic<bool> listening = false;
    struct Heard {
        rush2::online::LanLobby lobby;
        clock_type::time_point when;
    };
    std::map<std::string, Heard> heard;  // By "address:port".

    void listen_loop() {
        ENetSocket socket = enet_socket_create(ENET_SOCKET_TYPE_DATAGRAM);
        if (socket == ENET_SOCKET_NULL) {
            listening = false;
            return;
        }
        enet_socket_set_option(socket, ENET_SOCKOPT_REUSEADDR, 1);
        ENetAddress any{};
        any.host = ENET_HOST_ANY;
        any.port = rush2::online::discovery_port;
        if (enet_socket_bind(socket, &any) != 0) {
            enet_socket_destroy(socket);
            listening = false;
            return;
        }
        const std::string key = rush2::online::compatibility_key();
        uint8_t buffer[1500];
        while (listening) {
            enet_uint32 wait = ENET_SOCKET_WAIT_RECEIVE;
            if (enet_socket_wait(socket, &wait, 250) != 0 || !(wait & ENET_SOCKET_WAIT_RECEIVE)) {
                continue;
            }
            ENetAddress from{};
            ENetBuffer buf{};
            buf.data = buffer;
            buf.dataLength = sizeof(buffer);
            int got = enet_socket_receive(socket, &from, &buf, 1);
            if (got < int(sizeof(magic)) || std::memcmp(buffer, magic, sizeof(magic)) != 0) {
                continue;
            }
            rush2::online::Reader r(buffer + sizeof(magic), size_t(got) - sizeof(magic));
            uint32_t protocol = r.u32();
            std::string their_key = r.str();
            rush2::online::LanLobby lobby;
            lobby.name = r.str();
            lobby.port = r.u16();
            lobby.players = r.u8();
            lobby.max_players = r.u8();
            lobby.racing = r.u8() != 0;
            lobby.settings = r.settings();
            if (r.bad) {
                continue;
            }
            char ip[64] = {};
            enet_address_get_host_ip(&from, ip, sizeof(ip));
            lobby.address = ip;
            lobby.compatible = protocol == rush2::online::protocol_version && their_key == key;
            std::lock_guard lock{ listen_mutex };
            heard[lobby.address + ":" + std::to_string(lobby.port)] = { lobby, clock_type::now() };
        }
        enet_socket_destroy(socket);
    }
}

std::vector<uint8_t> rush2::online::beacon_payload(const std::string& name, uint16_t port, int players, int max_players,
    const RaceSettings& settings, bool racing) {
    Writer w;
    for (char c : magic) {
        w.u8(uint8_t(c));
    }
    w.u32(protocol_version);
    w.str(compatibility_key());
    w.str(name);
    w.u16(port);
    w.u8(uint8_t(players));
    w.u8(uint8_t(max_players));
    w.u8(racing ? 1 : 0);
    w.settings(settings);
    return w.data;
}

void rush2::online::send_beacon(const std::vector<uint8_t>& payload) {
    if (send_socket == ENET_SOCKET_NULL) {
        send_socket = enet_socket_create(ENET_SOCKET_TYPE_DATAGRAM);
        if (send_socket == ENET_SOCKET_NULL) {
            return;
        }
        enet_socket_set_option(send_socket, ENET_SOCKOPT_BROADCAST, 1);
    }
    ENetBuffer buf{};
    buf.data = const_cast<uint8_t*>(payload.data());
    buf.dataLength = payload.size();
    ENetAddress to{};
    to.port = discovery_port;
    to.host = ENET_HOST_BROADCAST;
    enet_socket_send(send_socket, &to, &buf, 1);
    enet_address_set_host_ip(&to, "127.0.0.1");
    enet_socket_send(send_socket, &to, &buf, 1);
}

void rush2::online::stop_beacons() {
    if (send_socket != ENET_SOCKET_NULL) {
        enet_socket_destroy(send_socket);
        send_socket = ENET_SOCKET_NULL;
    }
}

std::vector<rush2::online::LanLobby> rush2::online::lan_lobbies() {
    if (!available()) {
        return {};
    }
    if (!listening.exchange(true)) {
        if (listen_thread.joinable()) {
            listen_thread.join();
        }
        listen_thread = std::thread{ listen_loop };
    }
    std::vector<LanLobby> out;
    std::lock_guard lock{ listen_mutex };
    auto now = clock_type::now();
    for (auto it = heard.begin(); it != heard.end();) {
        if (now - it->second.when > lobby_lifetime) {
            it = heard.erase(it);
            continue;
        }
        ++it;
    }
    for (const auto& [key, h] : heard) {
        // A lobby on this computer is heard both from its LAN address and from 127.0.0.1: keep the LAN one.
        if (h.lobby.address == "127.0.0.1") {
            bool also_lan = false;
            for (const auto& [key2, h2] : heard) {
                also_lan |= h2.lobby.address != "127.0.0.1" && h2.lobby.port == h.lobby.port && h2.lobby.name == h.lobby.name;
            }
            if (also_lan) {
                continue;
            }
        }
        out.push_back(h.lobby);
    }
    return out;
}
