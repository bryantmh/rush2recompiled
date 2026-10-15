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
#include "recompui/recompui.h"
#include "librecomp/config.hpp"
#include "elements/ui_button.h"
#include "elements/ui_label.h"

#include "battle.h"
#include "data_files.h"
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

    // Sets this machine's settings to `text`'s ("config/option=value" lines), and writes the configs if `save`.
    void apply_settings(const std::string& text, bool save) {
        std::vector<recomp::config::Config*> changed;
        for (const auto& [key, value] : parse_settings(text)) {
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
        if (save) {
            for (recomp::config::Config* config : changed) {
                config->save_config();
            }
        }
    }

    // While a joined lobby's settings are the host's, this machine's own are kept in players.json's "online_backup"
    // section ({ "settings": lines }), so they come back on leaving, or at the next start if the game closed first.
    const std::string backup_section = "online_backup";
    bool following_host = false;

    std::string read_backup() {
        std::string text = rush2::data_files::read(rush2::data_files::File::Players, backup_section);
        if (text.empty()) {
            return {};
        }
        nlohmann::json j = nlohmann::json::parse(text, nullptr, false);
        if (j.is_discarded() || !j.is_object() || !j.contains("settings") || !j["settings"].is_string()) {
            return {};
        }
        return j["settings"].get<std::string>();
    }

    void write_backup(const std::string& settings) {
        nlohmann::json j = nlohmann::json::object();
        if (!settings.empty()) {
            j["settings"] = settings;
        }
        rush2::data_files::write(rush2::data_files::File::Players, backup_section, j.dump());
    }

    // Puts this machine's own settings back (and writes them), if they were set aside.
    void restore_own_settings() {
        std::string own = read_backup();
        if (!own.empty()) {
            apply_settings(own, true);
            write_backup({});
        }
        following_host = false;
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
            "The host's address, as their Online tab shows it while they host: for example 192.168.1.20 on your "
            "network or 203.0.113.7 over the internet, with \":port\" if they changed the port.", "");
        online_config.add_string_option(ids::port, "Port",
            "The UDP port your lobby uses (1024-65535). Leave it at " + std::to_string(online::default_port) +
            " unless something else on this computer uses it. Players joining over the internet need it open on "
            "your router: the game asks the router to open it when you host. Takes effect the next time you host.",
            std::to_string(online::default_port));
        online_config.add_number_option(ids::max_players, "Max Players",
            "How many players your lobby takes, you included. Takes effect the next time you host.",
            2, online::max_players, 1, 0, false, online::max_players);
        for (const std::string& id : { ids::name }) {
            online_config.add_option_change_callback(id,
                [](recomp::config::ConfigValueVariant, recomp::config::ConfigValueVariant, recomp::config::OptionChangeContext) {
                    apply_choices();
                    online_config.save_config();
                });
        }
        for (const std::string& id : { ids::address, ids::port, ids::max_players }) {
            online_config.add_option_change_callback(id,
                [](recomp::config::ConfigValueVariant, recomp::config::ConfigValueVariant, recomp::config::OptionChangeContext) {
                    online_config.save_config();
                });
        }
    }

    const std::string description =
        "Race up to 8 players on separate machines, on your local network or over the internet.<br/><br/>"
        "<recomp-color primary>Host</recomp-color> a lobby and share your address, or "
        "<recomp-color primary>Join</recomp-color> one from the list of lobbies on your network or by its host's "
        "address. Then everything happens in the game: the host goes to Start Game (One Race, Stunt or Battle) and "
        "everyone's game follows it. Each player picks their own player and car; the track select is the host's."
        "<br/><br/>"
        "Everyone needs the same version of this app and the same game ROMs.";

    const char* progress_text(uint8_t p) {
        switch (online::Progress(p)) {
            case online::Progress::Lobby: return "In the lobby";
            case online::Progress::ChoosingPlayer: return "Choosing a player";
            case online::Progress::TrackSelect: return "At the track select";
            case online::Progress::ChoosingCar: return "Picking a car";
            case online::Progress::CarChosen: return "Ready";
        }
        return "";
    }

    // The page shows one of these, by the session's state.
    enum class View { Offline, Connecting, Lobby };

    View current_view() {
        switch (online::state()) {
            case online::State::Offline: return View::Offline;
            case online::State::Connecting: return View::Connecting;
            default: return View::Lobby;
        }
    }

    class OnlinePage : public rush2::ui::OptionsPage {
    public:
        OnlinePage(ResourceId rid, Element* parent) : OptionsPage(rid, parent, description) {
            ContextId context = get_current_context();

            // Why the last host or join failed, or the session ended; shown while not in a lobby.
            error_box = make_box(context);
            error_label = context.create_element<Label>(error_box, "", theme::Typography::Body);
            error_label->set_color(theme::color::DangerL);
            error_box->set_padding_top(4.0f);

            // In a lobby (or connecting to one).
            const std::string lobby_text = "Close Lobby ends your lobby for everyone in it.<br/><br/>"
                "While you're in someone else's lobby the race uses the host's gameplay settings (the General tab's "
                "Gameplay options, Games, Cheats and AI Opponents). Yours come back when you leave.";
            lobby_heading = add_heading("Lobby", "", lobby_text);
            lobby_heading.row->set_padding_top(4.0f);
            leave_row = add_row(lobby_text);
            leave_button = context.create_element<Button>(leave_row, "Leave Lobby", ButtonStyle::Secondary);
            leave_button->add_pressed_callback([]() { online::leave(); });
            invite_heading = add_heading("Invite Players", "",
                "Give players the address for where they are. On your network they can also pick your lobby from "
                "their Online tab's list.");
            invite_box = make_box(context);
            players_heading = add_heading("Players", "", "Everyone in the lobby and how far through the host's "
                "menus they are.");
            players_box = make_box(context);
            lobby_items = { lobby_heading.row, leave_row, invite_heading.row, invite_box, players_heading.row,
                players_box };

            // Not in a lobby: host one or join one.
            host_heading = add_heading("Host a Lobby", "", "Open a lobby on this computer for others to join.");
            host_heading.row->set_padding_top(4.0f);
            Element* host_row = add_row("Opens your lobby. Players on your network see it in their list; players "
                "over the internet join with the address shown once it's open.");
            host_button = context.create_element<Button>(host_row, "Host Lobby", ButtonStyle::Primary);
            host_button->add_pressed_callback([]() {
                apply_choices();
                online::HostOptions options;
                options.port = host_port();
                options.max_players = get_number(ids::max_players);
                online::host(options);
            });
            add_option(online_config, ids::max_players);
            add_option(online_config, ids::port);
            port_status = add_option_status(ids::port);

            lan_heading = add_heading("Join a Lobby", "", "Lobbies hosted on your network show up here within a few "
                "seconds of opening. For a lobby over the internet, enter its host's address below.");
            lan_box = make_box(context);
            add_option(online_config, ids::address);
            Element* join_row = add_row("Joins the lobby at the address above.");
            join_button = context.create_element<Button>(join_row, "Join by Address", ButtonStyle::Secondary);
            join_button->add_pressed_callback([]() {
                apply_choices();
                online::join(get_string(ids::address));
            });
            offline_items = { error_box, host_heading.row, host_row, get_option_row(ids::max_players),
                get_option_row(ids::port), lan_heading.row, lan_box, get_option_row(ids::address), join_row };

            render_lan();
            on_update();
        }

    protected:
        std::string_view get_type_name() override { return "OnlinePage"; }

        void on_update() override {
            View view = current_view();
            if (view != shown_view) {
                // The focused button may have just been hidden: the controller goes to the new view's first button,
                // and the description pane back to the page's (not on the first frame: the tab bar keeps focus).
                bool switched = shown_view != View(-1);
                shown_view = view;
                for (Element* e : offline_items) {
                    if (e != nullptr) e->set_display(view == View::Offline ? Display::Flex : Display::None);
                }
                for (Element* e : lobby_items) {
                    e->set_display(view == View::Offline ? Display::None : Display::Flex);
                }
                rendered_lobby.clear();
                shown_text.clear();
                if (switched) {
                    (view == View::Offline ? host_button : leave_button)->focus();
                    text_shower(description)();
                }
            }
            if (view == View::Offline) {
                update_offline();
            }
            else {
                update_lobby(view);
            }
            if (++frames % 30 == 0) {
                // Settings changed on other tabs reach the lobby while this one is open.
                online::set_local_gameplay(gameplay_settings());
            }
        }

    private:
        Heading lobby_heading{};
        Heading invite_heading{};
        Heading players_heading{};
        Heading lan_heading{};
        Heading host_heading{};
        Element* error_box = nullptr;
        Label* error_label = nullptr;
        Element* leave_row = nullptr;
        Button* leave_button = nullptr;
        Element* invite_box = nullptr;
        Element* players_box = nullptr;
        Element* lan_box = nullptr;
        Button* join_button = nullptr;
        Button* host_button = nullptr;
        Label* port_status = nullptr;
        std::vector<Element*> offline_items;
        std::vector<Element*> lobby_items;
        View shown_view = View(-1);
        std::string rendered_lobby;
        std::string rendered_lan;
        std::string shown_text;
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

        // A row in a box that scrolls into view as its buttons take focus; text shows on the right while it's hovered.
        Element* make_row(ContextId context, Element* box, const std::string& text) {
            Element* row = context.create_element<rush2::ui::FocusRow>(box, text_shower(text));
            row->set_display(Display::Flex);
            row->set_flex_direction(FlexDirection::Row);
            row->set_align_items(AlignItems::Center);
            row->set_gap(16.0f);
            row->set_as_navigation_container(NavigationType::Horizontal);
            return row;
        }

        // A label in a row; width > 0 makes it a fixed column.
        static Label* add_cell(ContextId context, Element* row, const std::string& text, float width = 0.0f,
            theme::color color = theme::color::Text) {
            Label* label = context.create_element<Label>(row, text, theme::Typography::Body);
            if (width > 0.0f) {
                label->set_width(width);
                label->set_flex_shrink(0.0f);
            }
            label->set_color(color);
            return label;
        }

        static void set_note(Label* note, const std::string& text) {
            note->set_text(text);
            note->set_display(text.empty() ? Display::None : Display::Block);
        }

        void update_offline() {
            std::string error = online::last_error();
            error_box->set_display(error.empty() ? Display::None : Display::Flex);
            host_heading.row->set_padding_top(error.empty() ? 4.0f : 16.0f);
            if (error != shown_text) {
                shown_text = error;
                error_label->set_text(error);
            }
            int port = std::atoi(get_string(ids::port).c_str());
            port_status->set_text(port >= 1024 && port <= 65535 ? "" :
                "Not a port: uses " + std::to_string(online::default_port));
            if (frames % 30 == 0 && lan_signature() != rendered_lan) {
                render_lan();
            }
        }

        void update_lobby(View view) {
            bool host = online::is_host();
            std::string title = view == View::Connecting ? "Connecting..." :
                (host ? "Hosting " : "In ") + online::lobby_name();
            std::string note;
            switch (online::state()) {
                case online::State::Lobby:
                    note = host ? "Go to Start Game in the game to pick a race." :
                        "Waiting for the host to start a race.";
                    break;
                case online::State::Loading: note = "Starting the race..."; break;
                case online::State::Racing: note = "Racing."; break;
                default: break;
            }
            std::string error = online::last_error();
            if (view == View::Lobby && !error.empty()) {
                note += "  " + error;
            }
            std::string text = title + "|" + note;
            if (text != shown_text) {
                shown_text = text;
                lobby_heading.title->set_text(title);
                set_note(lobby_heading.note, note);
                leave_button->set_text(view == View::Connecting ? "Cancel" : host ? "Close Lobby" : "Leave Lobby");
            }
            if (lobby_signature() != rendered_lobby) {
                render_lobby(view);
            }
        }

        static std::string lobby_signature() {
            std::string s = std::to_string((int)online::state()) + (online::is_host() ? "h" : "j") +
                std::to_string(online::local_slot()) + std::to_string((int)online::router_port()) +
                online::router_status() + online::public_address();
            for (const online::RosterEntry& e : online::roster()) {
                s += "|" + std::to_string(e.used) + std::to_string(e.connected) + std::to_string(e.progress) + e.name +
                    std::to_string(e.ping_ms / 20);
            }
            return s;
        }

        void render_lobby(View view) {
            ContextId context = get_current_context();
            rendered_lobby = lobby_signature();
            bool host = online::is_host();
            bool joined = view == View::Lobby;
            invite_heading.row->set_display(host && joined ? Display::Flex : Display::None);
            invite_box->set_display(host && joined ? Display::Flex : Display::None);
            players_heading.row->set_display(joined ? Display::Flex : Display::None);
            players_box->set_display(joined ? Display::Flex : Display::None);

            invite_box->clear_children();
            if (host && joined) {
                std::string addresses;
                for (const std::string& a : online::local_addresses()) {
                    addresses += (addresses.empty() ? "" : ", ") + a;
                }
                uint16_t port = online::lobby_port();
                std::string port_suffix = port == online::default_port ? "" : ":" + std::to_string(port);
                Element* lan = make_row(context, invite_box, "Players on your network join with this address, or "
                    "pick your lobby from their list.");
                add_cell(context, lan, "Your network", 160.0f, theme::color::TextDim);
                add_cell(context, lan, addresses.empty() ? "unknown" : addresses + port_suffix);

                std::string router = online::router_status();
                Element* net = make_row(context, invite_box, router.empty() ? "Players over the internet join with "
                    "this address." : router);
                add_cell(context, net, "Internet", 160.0f, theme::color::TextDim);
                std::string internet = online::public_address();
                add_cell(context, net, internet.empty() ? "unknown" : internet + port_suffix, 240.0f);
                switch (online::router_port()) {
                    case online::RouterPort::Checking:
                        add_cell(context, net, "Opening the port...", 0.0f, theme::color::TextDim);
                        break;
                    case online::RouterPort::Open:
                        add_cell(context, net, "Port open", 0.0f, theme::color::SuccessL);
                        break;
                    case online::RouterPort::Unreachable:
                        add_cell(context, net, "Can't be reached", 0.0f, theme::color::WarningL);
                        break;
                    case online::RouterPort::Closed:
                        add_cell(context, net, "Port closed", 0.0f, theme::color::WarningL);
                        break;
                    case online::RouterPort::None: break;
                }
                // What to do about it, where a controller player can read it too.
                online::RouterPort rp = online::router_port();
                if (rp == online::RouterPort::Closed || rp == online::RouterPort::Unreachable) {
                    Element* fix = make_row(context, invite_box, router);
                    add_cell(context, fix, "", 160.0f);
                    add_cell(context, fix, rp == online::RouterPort::Closed ?
                        "To host over the internet, forward UDP port " + std::to_string(port) + " to " +
                        (addresses.empty() ? std::string("this computer") : addresses) + " on your router." :
                        "Your router is behind another network: players on the internet can't join. Another player "
                        "can host instead.", 0.0f, theme::color::TextDim);
                }
            }

            players_box->clear_children();
            if (!joined) {
                return;
            }
            std::vector<online::RosterEntry> players = online::roster();
            int me = online::local_slot();
            int count = 0;
            bool racing = online::state() == online::State::Racing;
            for (size_t i = 0; i < players.size(); i++) {
                const online::RosterEntry& e = players[i];
                if (!e.used) {
                    continue;
                }
                count++;
                Element* row = make_row(context, players_box, "");
                theme::color color = e.connected ? theme::color::Text : theme::color::TextDim;
                add_cell(context, row, e.name, 220.0f, color);
                add_cell(context, row, !e.connected ? "Left" : racing ? "Racing" : progress_text(e.progress), 200.0f,
                    color);
                std::string tag;
                if (i == 0) {
                    tag = (int)i == me ? "Host (you)" : "Host";
                }
                else if ((int)i == me) {
                    tag = "You";
                }
                else if (e.connected) {
                    tag = std::to_string(e.ping_ms) + " ms";
                }
                add_cell(context, row, tag, 0.0f, theme::color::TextDim);
            }
            set_note(players_heading.note, std::to_string(count) + " of " +
                std::to_string(online::max_players_setting()));
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
            set_note(lan_heading.note, lobbies.empty() ? "Searching your network..." :
                std::to_string(lobbies.size()) + (lobbies.size() == 1 ? " lobby on your network" :
                " lobbies on your network"));
            for (const online::LanLobby& l : lobbies) {
                std::string why = !l.compatible ? "Different app version or ROMs" : l.racing ? "Racing" :
                    l.players >= l.max_players ? "Full" : "";
                Element* row = make_row(context, lan_box, why.empty() ? "Joins " + l.name + " at " + l.address + "." :
                    l.name + " can't be joined right now: " + why + ".");
                Button* join = context.create_element<Button>(row, "Join", ButtonStyle::Secondary);
                join->set_enabled(why.empty());
                std::string address = l.address + ":" + std::to_string(l.port);
                join->add_pressed_callback([address]() {
                    if (online::state() == online::State::Offline) {
                        apply_choices();
                        online::join(address);
                    }
                });
                add_cell(context, row, l.name, 220.0f);
                add_cell(context, row, std::to_string(l.players) + "/" + std::to_string(l.max_players), 60.0f);
                add_cell(context, row, why, 0.0f, theme::color::TextDim);
            }
        }
    };
}

recomp::config::Config& rush2::online_tab::config() {
    return online_config;
}

void rush2::online_tab::load_config() {
    online_config.load_config();
    // The game closed in a lobby last time: this machine's own gameplay settings come back.
    restore_own_settings();
    apply_choices();
}

void rush2::online_tab::follow_host_settings() {
    static int frames = 0;
    if (++frames % 15 != 0 || recompui::is_context_capturing_input()) {
        return;  // A menu is open: the configs are the UI's.
    }
    online::State st = online::state();
    bool joined = !online::is_host() && st != online::State::Offline && st != online::State::Connecting;
    if (!joined) {
        if (following_host) {
            restore_own_settings();
            online::set_local_gameplay(gameplay_settings());
        }
        return;
    }
    // Not mid-race: the race runs with the settings it started with.
    std::vector<online::RosterEntry> players = online::roster();
    if (st != online::State::Lobby || players.empty() || !players[0].used || players[0].gameplay.empty()) {
        return;
    }
    std::string mine = gameplay_settings();
    if (mine != players[0].gameplay) {
        if (!following_host && read_backup().empty()) {
            write_backup(mine);
        }
        following_host = true;
        apply_settings(players[0].gameplay, false);
        mine = gameplay_settings();
    }
    online::set_local_gameplay(mine);
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
