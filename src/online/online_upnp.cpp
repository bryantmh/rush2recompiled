// Online play: opens the host's UDP port on the router with UPnP (miniupnpc), so players outside the LAN can join by
// the host's address without the host forwarding the port by hand (docs/online.md#hosting-over-the-internet). Runs on
// its own thread because discovering the router takes up to a few seconds. The mapping is removed when the lobby
// closes; a lease of an hour keeps a crashed game from leaving it open for good.

#include <atomic>
#include <cstdio>
#include <mutex>
#include <string>
#include <thread>

#define MINIUPNP_STATICLIB
#include "miniupnpc.h"
#include "upnpcommands.h"
#include "upnperrors.h"

#include "online_internal.h"

namespace {
    std::mutex upnp_mutex;
    std::thread upnp_thread;
    std::string status_text;
    std::string public_ip;

    // The mapping made, to delete it again.
    bool mapped = false;
    std::string control_url;
    std::string service_type;
    std::string mapped_port;

    void set_status(const std::string& text) {
        std::lock_guard lock{ upnp_mutex };
        status_text = text;
    }

    void delete_mapping() {
        if (mapped) {
            UPNP_DeletePortMapping(control_url.c_str(), service_type.c_str(), mapped_port.c_str(), "UDP", nullptr);
            mapped = false;
        }
    }

    void open_port(uint16_t port) {
        set_status("Looking for the router...");
        int error = 0;
        UPNPDev* devices = upnpDiscover(2000, nullptr, nullptr, 0, 0, 2, &error);
        if (devices == nullptr) {
            set_status("No router answered (UPnP off or not supported): forward UDP port " + std::to_string(port) +
                " to this computer to host over the internet.");
            return;
        }

        UPNPUrls urls{};
        IGDdatas data{};
        char lan_address[64] = {};
        char wan_address[64] = {};
        int found = UPNP_GetValidIGD(devices, &urls, &data, lan_address, sizeof(lan_address), wan_address, sizeof(wan_address));
        freeUPNPDevlist(devices);
        if (found != UPNP_CONNECTED_IGD && found != UPNP_PRIVATEIP_IGD) {
            if (found != UPNP_NO_IGD) {
                FreeUPNPUrls(&urls);
            }
            set_status("The router doesn't allow opening ports (UPnP): forward UDP port " + std::to_string(port) +
                " to this computer to host over the internet.");
            return;
        }

        std::string port_text = std::to_string(port);
        int result = UPNP_AddPortMapping(urls.controlURL, data.first.servicetype, port_text.c_str(), port_text.c_str(),
            lan_address, "Rush 2 Recompiled", "UDP", nullptr, "3600");
        if (result != UPNPCOMMAND_SUCCESS) {
            FreeUPNPUrls(&urls);
            set_status(std::string("The router refused to open the port (") + strupnperror(result) +
                "): forward UDP port " + port_text + " to this computer to host over the internet.");
            return;
        }

        char external[40] = {};
        UPNP_GetExternalIPAddress(urls.controlURL, data.first.servicetype, external);
        {
            std::lock_guard lock{ upnp_mutex };
            mapped = true;
            control_url = urls.controlURL;
            service_type = data.first.servicetype;
            mapped_port = port_text;
            public_ip = external;
        }
        FreeUPNPUrls(&urls);

        if (found == UPNP_PRIVATEIP_IGD) {
            // The router's own internet address is private: another router or the internet provider sits in between
            // (carrier-grade NAT), so the opened port isn't reachable from the internet.
            set_status("Port opened on the router, but the router is behind another network (its address " +
                std::string(external) + " is private), so players outside the LAN can't reach this computer. "
                "Another player can host instead.");
        }
        else {
            set_status("Port " + port_text + " opened on the router. Players outside the LAN join with " +
                std::string(external) + (port == rush2::online::default_port ? "" : ":" + port_text) + ".");
        }
    }
}

void rush2::online::upnp_open(uint16_t port) {
    upnp_close();
    upnp_thread = std::thread{ [port]() { open_port(port); } };
}

void rush2::online::upnp_close() {
    if (upnp_thread.joinable()) {
        upnp_thread.join();
    }
    std::lock_guard lock{ upnp_mutex };
    delete_mapping();
    status_text.clear();
    public_ip.clear();
}

std::string rush2::online::upnp_status() {
    std::lock_guard lock{ upnp_mutex };
    return status_text;
}

std::string rush2::online::upnp_public_address() {
    std::lock_guard lock{ upnp_mutex };
    return public_ip;
}
