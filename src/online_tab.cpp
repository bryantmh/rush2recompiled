// Online tab (docs/online.md#the-online-tab): hosting a lobby, joining one by address or from the LAN list, the lobby's
// players, and the host's race settings. The choices that persist (name, last address, host settings, car) are the
// "online" config, online.json; the display name is also on the Controls tab. Everything here takes effect at once.

// As in src/rush2049/wings.cpp: librecomp's nlohmann::json first, so Config::load_config links.
#include "../lib/N64ModernRuntime/thirdparty/json/json.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "recompui/config.h"
#include "librecomp/config.hpp"
#include "elements/ui_button.h"
#include "elements/ui_label.h"

#include "battle.h"
#include "npc_cars.h"
#include "online.h"
#include "options_page.h"
#include "wings_internal.h"
#include "track1.h"
#include "track2049.h"

namespace {
    using namespace recompui;
    namespace online = rush2::online;

    const std::string tab_id = "online";
    const std::string tab_name = "Online";
    const std::string config_id = "online";
    recomp::config::Config online_config{ "Online", config_id, false };

    namespace ids {
        const std::string name = "display_name";
        const std::string address = "join_address";
        const std::string port = "host_port";
        const std::string max_players = "host_max_players";
        const std::string upnp = "host_open_router_port";
    }

    std::string get_string(const std::string& id) {
        return std::get<std::string>(online_config.get_option_value(id));
    }
    uint32_t get_enum(const std::string& id) {
        return std::get<uint32_t>(online_config.get_option_value(id));
    }
    int get_number(const std::string& id) {
        return (int)std::get<double>(online_config.get_option_value(id));
    }
    bool get_bool(const std::string& id) {
        return std::get<bool>(online_config.get_option_value(id));
    }
    uint16_t host_port() {
        int port = std::atoi(get_string(ids::port).c_str());
        return port >= 1024 && port <= 65535 ? (uint16_t)port : online::default_port;
    }

    // The settings that change the race, which every player of an online race needs the same (the race runs on every
    // machine from the inputs alone): the General tab's Gameplay options that change the cars or the computer cars,
    // and all of the Games (Rush 2049 options), Cheats and AI Opponents settings.
    struct GameplayConfig {
        const char* key;
        recomp::config::Config& (*get)();
        std::vector<std::string> only;  // These options only; empty for all.
    };
    const GameplayConfig gameplay_configs[] = {
        { "general", []() -> recomp::config::Config& { return recompui::config::get_config("general"); },
          { "accurate_car_stats", "torque_rebalance", "stunt_first_gear", "drone_catch_up" } },
        { "games", []() -> recomp::config::Config& { return rush2::wings::games_config(); }, {} },
        { "cheats", []() -> recomp::config::Config& { return recompui::config::get_config("cheats"); }, {} },
        { "npc_cars", []() -> recomp::config::Config& { return rush2::npc_cars::config(); }, {} },
    };

    std::string value_text(const recomp::config::ConfigValueVariant& v) {
        if (const bool* b = std::get_if<bool>(&v)) return *b ? "1" : "0";
        if (const uint32_t* u = std::get_if<uint32_t>(&v)) return std::to_string(*u);
        if (const double* d = std::get_if<double>(&v)) {
            char text[32];
            std::snprintf(text, sizeof(text), "%g", *d);
            return text;
        }
        if (const std::string* s = std::get_if<std::string>(&v)) return *s;
        return {};
    }

    // "config/option=value" lines.
    std::string gameplay_settings() {
        std::string out;
        for (const GameplayConfig& g : gameplay_configs) {
            recomp::config::Config& config = g.get();
            for (const recomp::config::ConfigOption& o : config.get_config_schema().options) {
                if (!g.only.empty() && std::find(g.only.begin(), g.only.end(), o.id) == g.only.end()) {
                    continue;
                }
                out += std::string(g.key) + "/" + o.id + "=" + value_text(config.get_option_value(o.id)) + "\n";
            }
        }
        return out;
    }

    std::vector<std::pair<std::string, std::string>> parse_settings(const std::string& text) {
        std::vector<std::pair<std::string, std::string>> out;
        size_t at = 0;
        while (at < text.size()) {
            size_t end = text.find('\n', at);
            std::string line = text.substr(at, end == std::string::npos ? std::string::npos : end - at);
            at = end == std::string::npos ? text.size() : end + 1;
            size_t eq = line.find('=');
            if (eq != std::string::npos) {
                out.emplace_back(line.substr(0, eq), line.substr(eq + 1));
            }
        }
        return out;
    }

    // The config and option of a "config/option" key, or null.
    recomp::config::Config* find_option(const std::string& key, std::string& option_id) {
        size_t slash = key.find('/');
        for (const GameplayConfig& g : gameplay_configs) {
            if (key.compare(0, slash, g.key) == 0) {
                recomp::config::Config& config = g.get();
                option_id = key.substr(slash + 1);
                return config.has_option(option_id) ? &config : nullptr;
            }
        }
        return nullptr;
    }

    // The names of the settings that differ between two players' settings.
    std::vector<std::string> differences(const std::string& mine, const std::string& theirs) {
        std::vector<std::string> names;
        auto a = parse_settings(mine);
        auto b = parse_settings(theirs);
        for (const auto& [key, value] : b) {
            auto it = std::find_if(a.begin(), a.end(), [&](const auto& p) { return p.first == key; });
            if (it != a.end() && it->second == value) {
                continue;
            }
            std::string id;
            recomp::config::Config* config = find_option(key, id);
            names.push_back(config != nullptr ? config->get_option(id).name : key);
        }
        if (a.size() != b.size() && names.empty()) {
            names.push_back("(a different set of settings)");
        }
        return names;
    }

    // Sets this machine's settings to the host's (the joiner's choice, from the lobby).
    void match_settings(const std::string& host) {
        std::vector<recomp::config::Config*> changed;
        for (const auto& [key, value] : parse_settings(host)) {
            std::string id;
            recomp::config::Config* config = find_option(key, id);
            if (config == nullptr) {
                continue;
            }
            recomp::config::ConfigValueVariant cur = config->get_option_value(id);
            if (value_text(cur) == value) {
                continue;
            }
            recomp::config::ConfigValueVariant v;
            if (std::holds_alternative<bool>(cur)) v = value == "1";
            else if (std::holds_alternative<uint32_t>(cur)) v = (uint32_t)std::strtoul(value.c_str(), nullptr, 10);
            else if (std::holds_alternative<double>(cur)) v = std::strtod(value.c_str(), nullptr);
            else v = value;
            config->update_option_value(id, v);
            if (std::find(changed.begin(), changed.end(), config) == changed.end()) {
                changed.push_back(config);
            }
        }
        for (recomp::config::Config* config : changed) {
            config->save_config();
        }
        online::set_local_gameplay(gameplay_settings());
    }

    uint8_t local_games() {
        return (rush2::track2049::available() ? online::games_rush2049 : 0) |
            (rush2::track1::available() ? online::games_rush1 : 0);
    }

    // Pushes this machine's choices to the session (and, hosting, the race settings to everyone).
    void apply_choices() {
        online::set_display_name(get_string(ids::name));
        online::set_local_games(local_games());
        online::set_local_gameplay(gameplay_settings());
    }

    void add_options() {
        online_config.add_string_option(ids::name, "Online Name",
            "Your name in online lobbies (up to 16 letters). Takes effect at once.", "Player");
        online_config.add_string_option(ids::address, "Host Address",
            "The address of the lobby to join: the host's IP address as they read it from their Online tab, with "
            "\":port\" if they changed the port (for example 203.0.113.7 or 192.168.1.20:47100).", "");
        online_config.add_string_option(ids::port, "Port",
            "The UDP port your lobby listens on (1024-65535). Players joining over the internet need it open on your "
            "router (Open Router Port does that when the router allows it). Takes effect the next time you host.",
            std::to_string(online::default_port));
        online_config.add_number_option(ids::max_players, "Max Players",
            "How many players your lobby takes, you included. Takes effect the next time you host.",
            2, online::max_players, 1, 0, false, online::max_players);
        online_config.add_bool_option(ids::upnp, "Open Router Port",
            "Asks your router (UPnP) to forward the port to this machine while you host, so players can join over "
            "the internet. Not every router allows it; the lobby shows whether it worked. Takes effect the next time "
            "you host.", true);
        for (const std::string& id : { ids::name }) {
            online_config.add_option_change_callback(id,
                [](recomp::config::ConfigValueVariant, recomp::config::ConfigValueVariant, recomp::config::OptionChangeContext) {
                    apply_choices();
                    online_config.save_config();
                });
        }
        for (const std::string& id : { ids::address, ids::port, ids::max_players, ids::upnp }) {
            online_config.add_option_change_callback(id,
                [](recomp::config::ConfigValueVariant, recomp::config::ConfigValueVariant, recomp::config::OptionChangeContext) {
                    online_config.save_config();
                });
        }
    }

    const std::string description =
        "Race up to 8 players on separate machines, on your local network or over the internet.<br/><br/>"
        "<recomp-color primary>Host</recomp-color> a lobby and share your address, or "
        "<recomp-color primary>Join</recomp-color> one by its host's address or from the list of lobbies on your "
        "network. Then everything happens in the game: the host goes to Start Game (One Race, Stunt or Battle) and "
        "everyone's game follows it. Each player picks their own player and car; the track select is the host's."
        "<br/><br/>"
        "Everyone needs the same version of this app and the same game ROMs.";

    const char* progress_text(uint8_t p) {
        switch (online::Progress(p)) {
            case online::Progress::Lobby: return "in the lobby";
            case online::Progress::ChoosingPlayer: return "choosing a player";
            case online::Progress::TrackSelect: return "at the track select";
            case online::Progress::ChoosingCar: return "picking a car";
            case online::Progress::CarChosen: return "ready";
        }
        return "";
    }

    std::string state_text() {
        switch (online::state()) {
            case online::State::Offline: {
                std::string error = online::last_error();
                return error.empty() ? "Not in a lobby" : error;
            }
            case online::State::Connecting: return "Connecting...";
            case online::State::Lobby: {
                std::string error = online::last_error();
                return (online::is_host() ? "Hosting \"" + online::lobby_name() + "\"" :
                    "In \"" + online::lobby_name() + "\"") + (error.empty() ? "" : "  -  " + error);
            }
            case online::State::Loading: return "Starting the race...";
            case online::State::Racing: return "Racing";
        }
        return {};
    }

    class OnlinePage : public rush2::ui::OptionsPage {
    public:
        OnlinePage(ResourceId rid, Element* parent) : OptionsPage(rid, parent, description) {
            ContextId context = get_current_context();

            lobby_heading = add_heading("Lobby", state_text(), description);
            lobby_box = make_box(context);
            render_lobby();

            add_heading("Join", "", "Join a lobby by its host's address.");
            add_option(online_config, ids::address);
            Element* join_row = add_row("Joins the lobby at the address above.");
            join_button = context.create_element<Button>(join_row, "Join", ButtonStyle::Primary);
            join_button->add_pressed_callback([]() {
                apply_choices();
                online::join(get_string(ids::address));
            });

            lan_heading = add_heading("Lobbies on Your Network", "Looking...",
                "Lobbies hosted on your local network. They show up within a few seconds of being hosted.");
            lan_box = make_box(context);
            render_lan();

            add_heading("Host", "", "Your lobby's settings. Everyone joining needs to reach this machine's port.");
            add_option(online_config, ids::port);
            add_option(online_config, ids::max_players);
            add_option(online_config, ids::upnp);
            Element* host_row = add_row("Opens a lobby others can join.");
            host_button = context.create_element<Button>(host_row, "Host Lobby", ButtonStyle::Primary);
            host_button->add_pressed_callback([]() {
                apply_choices();
                online::HostOptions options;
                options.port = host_port();
                options.max_players = get_number(ids::max_players);
                options.open_router_port = get_bool(ids::upnp);
                online::host(options);
            });
            update_buttons();
        }

    protected:
        std::string_view get_type_name() override { return "OnlinePage"; }

        void on_update() override {
            std::string text = state_text();
            if (text != shown_state) {
                shown_state = text;
                lobby_heading.note->set_text(text);
            }
            if (lobby_signature() != rendered_lobby) {
                render_lobby();
            }
            if (++frames % 30 == 0) {
                // Settings changed on other tabs reach the lobby while this one is open.
                online::set_local_gameplay(gameplay_settings());
                if (lan_signature() != rendered_lan) {
                    render_lan();
                }
            }
            update_buttons();
        }

    private:
        Heading lobby_heading{};
        Heading lan_heading{};
        Element* lobby_box = nullptr;
        Element* lan_box = nullptr;
        Button* join_button = nullptr;
        Button* host_button = nullptr;
        std::string shown_state;
        std::string rendered_lobby;
        std::string rendered_lan;
        int frames = 0;

        Element* make_box(ContextId context) {
            Element* box = context.create_element<Element>(get_list(), 0, "div", false);
            box->set_display(Display::Flex);
            box->set_flex_direction(FlexDirection::Column);
            box->set_padding_left(12.0f);
            box->set_padding_right(12.0f);
            box->set_padding_bottom(8.0f);
            box->set_gap(8.0f);
            box->set_as_navigation_container(NavigationType::Vertical);
            return box;
        }

        // A row of buttons in a box that scrolls into view as they take focus.
        Element* make_row(ContextId context, Element* box, const std::string& text) {
            Element* row = context.create_element<rush2::ui::FocusRow>(box, text_shower(text));
            row->set_display(Display::Flex);
            row->set_flex_direction(FlexDirection::Row);
            row->set_align_items(AlignItems::Center);
            row->set_gap(12.0f);
            row->set_as_navigation_container(NavigationType::Horizontal);
            return row;
        }

        void update_buttons() {
            bool offline = online::state() == online::State::Offline;
            join_button->set_enabled(offline);
            host_button->set_enabled(offline);
        }

        static std::string lobby_signature() {
            std::string s = std::to_string((int)online::state()) + (online::is_host() ? "h" : "j") +
                std::to_string(online::local_slot()) + online::router_status() + online::public_address();
            for (const online::RosterEntry& e : online::roster()) {
                s += "|" + std::to_string(e.used) + std::to_string(e.connected) + std::to_string(e.progress) + e.name +
                    std::to_string(e.car.type) + std::to_string(e.ping_ms / 20) + e.gameplay;
            }
            return s;
        }

        void render_lobby() {
            ContextId context = get_current_context();
            rendered_lobby = lobby_signature();
            lobby_box->clear_children();
            online::State st = online::state();
            if (st == online::State::Offline || st == online::State::Connecting) {
                return;
            }
            std::vector<online::RosterEntry> players = online::roster();
            int me = online::local_slot();
            bool settings_differ = false;
            for (size_t i = 0; i < players.size(); i++) {
                const online::RosterEntry& e = players[i];
                if (!e.used) {
                    continue;
                }
                std::string line = std::to_string(i + 1) + ". " + e.name;
                if ((int)i == me) {
                    line += "  (you)";
                }
                else if (!e.connected) {
                    line += "  (left)";
                }
                else if (i != 0) {
                    line += "  " + std::to_string(e.ping_ms) + " ms";
                }
                line += "  -  " + std::string(progress_text(e.progress));
                context.create_element<Label>(lobby_box, line, theme::Typography::Body);
                if (i != 0 && e.connected) {
                    std::vector<std::string> names = differences(players[0].gameplay, e.gameplay);
                    if (!names.empty()) {
                        std::string list;
                        for (const std::string& n : names) {
                            list += (list.empty() ? "" : ", ") + n;
                        }
                        Label* diff = context.create_element<Label>(lobby_box, "     Settings differ from the host's: " +
                            list, theme::Typography::Body);
                        diff->set_color(theme::color::Warning);
                        settings_differ |= (int)i == me;
                    }
                }
            }
            if (online::is_host()) {
                std::string addresses;
                for (const std::string& a : online::local_addresses()) {
                    addresses += (addresses.empty() ? "" : ", ") + a;
                }
                Label* lan = context.create_element<Label>(lobby_box, "On your network: " + addresses,
                    theme::Typography::Body);
                lan->set_color(theme::color::TextDim);
                std::string internet = online::public_address();
                Label* net = context.create_element<Label>(lobby_box, "Over the internet: " +
                    (internet.empty() ? std::string("unknown") : internet) + "  -  " + online::router_status(),
                    theme::Typography::Body);
                net->set_color(theme::color::TextDim);
            }

            Element* row = make_row(context, lobby_box, online::is_host() ? "Leave closes the lobby." :
                "Match Host Settings sets your gameplay settings to the host's. Leave leaves the lobby.");
            if (st == online::State::Lobby && !online::is_host() && settings_differ) {
                Button* match = context.create_element<Button>(row, "Match Host Settings", ButtonStyle::Secondary);
                std::string host = players[0].gameplay;
                match->add_pressed_callback([host]() { match_settings(host); });
            }
            Button* leave = context.create_element<Button>(row, "Leave", ButtonStyle::Secondary);
            leave->add_pressed_callback([]() { online::leave(); });
        }

        static std::string lan_signature() {
            std::string s;
            for (const online::LanLobby& l : online::lan_lobbies()) {
                s += l.address + ":" + std::to_string(l.port) + l.name + std::to_string(l.players) +
                    std::to_string(l.max_players) + std::to_string(l.compatible) + std::to_string(l.racing) + "|";
            }
            return s;
        }

        void render_lan() {
            ContextId context = get_current_context();
            rendered_lan = lan_signature();
            lan_box->clear_children();
            std::vector<online::LanLobby> lobbies = online::lan_lobbies();
            // Not this machine's own lobby.
            if (online::is_host()) {
                std::erase_if(lobbies, [](const online::LanLobby& l) { return l.name == online::lobby_name(); });
            }
            lan_heading.note->set_text(lobbies.empty() ? "None found yet" :
                std::to_string(lobbies.size()) + (lobbies.size() == 1 ? " lobby" : " lobbies"));
            for (const online::LanLobby& l : lobbies) {
                std::string text = l.name + "  (" + std::to_string(l.players) + "/" + std::to_string(l.max_players) +
                    ")";
                if (!l.compatible) {
                    text += "  -  different version or games";
                }
                else if (l.racing) {
                    text += "  -  racing";
                }
                Element* row = make_row(context, lan_box, "Joins " + l.name + " at " + l.address + ".");
                Button* join = context.create_element<Button>(row, "Join", ButtonStyle::Secondary);
                join->set_enabled(l.compatible && !l.racing);
                std::string address = l.address + ":" + std::to_string(l.port);
                join->add_pressed_callback([address]() {
                    if (online::state() == online::State::Offline) {
                        apply_choices();
                        online::join(address);
                    }
                });
                context.create_element<Label>(row, text, theme::Typography::Body);
            }
        }
    };
}

recomp::config::Config& rush2::online_tab::config() {
    return online_config;
}

void rush2::online_tab::load_config() {
    online_config.load_config();
    apply_choices();
}

void rush2::online_tab::create_tab() {
    add_options();
    recompui::config::create_tab(tab_name, tab_id,
        [](ContextId context, Element* parent) {
            context.create_element<OnlinePage>(parent);
        },
        nullptr,
        [](TabCloseContext) {
            online_config.save_config();
        });
}
