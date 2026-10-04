// Race music across Rush 2, SF Rush and Rush 2049: the Sound tab's song list, how a race picks its song, and song
// previews.
//
// Songs: the ones the three games play on their race tracks. Rush 2's eight (its MUSIC setting's 2-9, sequences
// 0x800CC394 = [3, 4, 0, 1, 7, 10, 2, 8]; per-track choice 0x800CC37C), SF Rush's nine race songs (its random table
// 0x800D2910, played through Rush 2's player as sequences 13-28, src/track1_audio.cpp) and the six songs of Rush
// 2049's race tracks (0x8010FFD4, played on the host, src/track2049_audio.cpp).
//
// Choice: Rush 2 picks a race's song in func_8008C370(2, setting), setting 1 being "per track" (byte 0x800D5775).
// Only that setting is taken over; the fixed-song settings and Off stay the game's. Each game's Race Music option
// chooses for that game's tracks: Original (the game's own choice, swapped for another song of that game, then of any
// game, when it is switched off), Shuffle (a random song of the game) or Shuffle All (a random song of any
// game). SF Rush offers only Original and Shuffle All: its own choice already is a random SF Rush song.
// Shuffles skip the song played last. A song of a game whose ROM or audio isn't available is skipped; with nothing
// left the game picks as it would by itself. A Rush 2 or SF Rush song replaces the sequence Rush 2 chose (at
// 0x8008C46C); a Rush 2049 song is queued on the 2049 player and the call turned into the "music off" case, whose
// stop command starts it (src/track2049_audio.cpp).
//
// Preview: the Sound tab's play buttons. The game keeps running while the menu is open, so the preview runs on its
// audio thread (func_800631A4, before it takes queued music commands): Rush 2 and SF Rush songs start with
// func_80061E68 (0 while it is still stopping or loading the old song, so the request waits; 2 if that song already
// plays), Rush 2049 songs on the host player, each stopping the other. Every music command the game queues
// (func_80062F50) is noted as the music the game wants; while a preview plays, its plays and stops are held (turned
// into a volume update, as are any still in the queue when the preview starts), so the game can't take the music
// back. Stopping the preview (or closing the tab) then plays what the game last asked for.

// As in src/wings.cpp: librecomp's nlohmann::json first.
#include "../lib/N64ModernRuntime/thirdparty/json/json.hpp"

#include <array>
#include <atomic>
#include <cstdio>
#include <mutex>
#include <random>
#include <string>
#include <vector>

#include "recompui/config.h"
#include "librecomp/config.hpp"
#include "elements/ui_config_page.h"
#include "elements/ui_icon_button.h"
#include "elements/ui_label.h"
#include "elements/ui_radio.h"
#include "elements/ui_slider.h"
#include "elements/ui_toggle.h"

#include "rush2_hooks.h"
#include "music.h"
#include "track1.h"
#include "track2049.h"
#include "car_engines.h"
#include "wings.h"

extern "C" void func_80061E10(uint8_t* rdram, recomp_context* ctx);   // Stops the song.
extern "C" void func_80061E68(uint8_t* rdram, recomp_context* ctx);   // Plays sequence $a0; 0 if busy.
extern "C" void func_800091B0(uint8_t* rdram, recomp_context* ctx);   // alSeqpGetState($a0)

namespace {
    using namespace recompui;

    constexpr uint32_t track_id = 0x8010C3F0;
    // The other cars' engine sounds (src/car_engines.cpp).
    constexpr const char* other_engines_id = "other_engines";
    constexpr const char* other_engines_description =
        "Hear the computer cars' engines around you, as Rush 2049 does (Rush 2 plays only your own).";
    constexpr uint32_t track_songs = 0x800CC37C;    // s16[12]: index into the song table per track.
    constexpr uint32_t song_table = 0x800CC394;     // s16[8]: sequence per Rush 2 race song.
    constexpr uint32_t requested_song = 0x800F9550; // Sequence func_80061E68 last queued for the loader.
    constexpr uint32_t sequence_player = 0x800FAEB4; // ALSeqPlayer*
    constexpr int al_playing = 1;

    enum class Game { Rush2, Rush1, Rush2049 };

    struct Song {
        Game game;
        int id;             // Rush 2: song table index. SF Rush and Rush 2049: the game's song number.
        const char* key;    // Option id suffix.
        const char* name;
        const char* plays_on;
    };

    // In each game's own order: Rush 2's MUSIC setting, SF Rush's random table, Rush 2049's tracks.
    constexpr std::array<Song, 25> songs = {{
        { Game::Rush2, 0, "r2_0", "Head Thumpin'", "Lower Manhattan" },
        { Game::Rush2, 1, "r2_1", "Tinkle Toon", "Las Vegas" },
        { Game::Rush2, 2, "r2_2", "Drums N Hula", "Honolulu" },
        { Game::Rush2, 3, "r2_3", "Low Rydin'", "Los Angeles" },
        { Game::Rush2, 4, "r2_4", "Doin' Tyme", "Alcatraz" },
        { Game::Rush2, 5, "r2_5", "14 Da Boyz", "Seattle" },
        { Game::Rush2, 6, "r2_6", "Amiga-ish", "Upper Manhattan" },
        { Game::Rush2, 7, "r2_7", "High Roll", "Halfpipe, Crash, Pipe, Atari and Stunt 1" },
        { Game::Rush1, 0, "r1_0", "Song 1", "Any SF Rush track" },
        { Game::Rush1, 1, "r1_1", "Song 2", "Any SF Rush track" },
        { Game::Rush1, 2, "r1_2", "Song 3", "Any SF Rush track" },
        { Game::Rush1, 7, "r1_7", "Song 4", "Any SF Rush track" },
        { Game::Rush1, 3, "r1_3", "Song 5", "Any SF Rush track" },
        { Game::Rush1, 10, "r1_10", "Song 6", "Any SF Rush track" },
        { Game::Rush1, 6, "r1_6", "Song 7", "Any SF Rush track" },
        { Game::Rush1, 12, "r1_12", "Song 8", "Any SF Rush track" },
        { Game::Rush1, 15, "r1_15", "Song 9", "Any SF Rush track" },
        { Game::Rush2049, 0, "r49_0", "Song 1", "Rush 2049 Track 1" },
        { Game::Rush2049, 1, "r49_1", "Song 2", "Rush 2049 Track 2" },
        { Game::Rush2049, 4, "r49_4", "Song 3", "Rush 2049 Track 3" },
        { Game::Rush2049, 2, "r49_2", "Song 4", "Rush 2049 Track 4" },
        { Game::Rush2049, 3, "r49_3", "Song 5", "Rush 2049 Track 5" },
        { Game::Rush2049, 7, "r49_7", "Song 6", "Rush 2049 Track 6" },
        { Game::Rush2049, 8, "r49_8", "Song 7", "Rush 2049 Stunt 1 and 2" },
        { Game::Rush2049, 9, "r49_9", "Song 8", "Rush 2049 Stunt 3 and 4" },
    }};
    constexpr int song_count = (int)songs.size();
    constexpr int first_rush1 = 8;      // Catalog index of SF Rush's first song.
    constexpr int first_rush2049 = 17;  // Catalog index of Rush 2049's first song (track 1's).

    const std::string tab_id = "rush2_sound";
    enum class Mode : uint32_t { Original, ShuffleByGame, ShuffleAll };

    std::string song_option_id(const Song& s) {
        return std::string("song_") + s.key;
    }

    constexpr Game games[3] = { Game::Rush2, Game::Rush1, Game::Rush2049 };

    std::string mode_option_id(Game g) {
        return g == Game::Rush2 ? "race_music_r2" : g == Game::Rush1 ? "race_music_r1" : "race_music_r49";
    }

    // The Race Music choices a game offers, in the order shown.
    std::vector<Mode> game_modes(Game g) {
        if (g == Game::Rush1) {
            return { Mode::Original, Mode::ShuffleAll };
        }
        return { Mode::Original, Mode::ShuffleByGame, Mode::ShuffleAll };
    }

    const char* mode_name(Mode m) {
        switch (m) {
            case Mode::Original: return "Original";
            case Mode::ShuffleByGame: return "Shuffle";
            default: return "Shuffle All";
        }
    }

    std::string game_option_id(Game g) {
        return g == Game::Rush2 ? "games_r2" : g == Game::Rush1 ? "games_r1" : "games_r49";
    }

    const char* game_name(Game g) {
        switch (g) {
            case Game::Rush2: return "Rush 2";
            case Game::Rush1: return "SF Rush";
            default: return "Rush 2049";
        }
    }

    // Settings as the game threads read them.
    std::array<std::atomic<uint32_t>, 3> modes{};      // By Game value: Race Music for the game's tracks.
    std::array<std::atomic_bool, song_count> enabled{};
    std::array<std::atomic_bool, 3> game_enabled{};   // By Game value: the game's switch above its songs.
    std::mutex choice_mutex;
    std::mt19937 rng{ std::random_device{}() };
    int last_song = -1;   // Catalog index of the last race song.

    // The race's Rush 2 or SF Rush sequence for func_8008C370's tail, or -1.
    std::atomic<int> race_sequence = -1;

    // Preview requests from the UI: a catalog index, stop_request, or none.
    constexpr int no_request = -2;
    constexpr int stop_request = -1;
    std::atomic<int> preview_request = no_request;
    std::atomic<int> previewing = -1;   // Catalog index the UI shows as playing.

    // Preview state (audio thread) and the music the game's commands last asked for (any thread that queues one);
    // both under preview_mutex.
    std::mutex preview_mutex;
    struct PreviewState {
        bool active = false;
        int song = -1;              // Catalog index playing.
        int sequence = -1;          // Its Rush 2 sequence, or -1 for a 2049 song.
    } preview;
    struct GameMusic {
        int sequence = -1;          // A Rush 2 sequence,
        int song_2049 = -1;         // or a Rush 2049 song (a race's "music off" stop that plays one), or neither.
    } game_music;

    // Music command queue (func_80062F50): 16 commands, read and write indices.
    constexpr uint32_t command_queue = 0x800D4AD0;
    constexpr uint32_t command_read = 0x800D43A8;
    constexpr uint32_t command_write = 0x800D4418;
    constexpr uint32_t command_volume = 0xC0000000;   // Applies the music volume and fade.
    constexpr uint32_t current_song = 0x8010BCD8;    // u8: the loaded sequence (0xFF none), then the one loading.

    bool game_available(Game g) {
        switch (g) {
            case Game::Rush2: return true;
            case Game::Rush1: return rush2::track1::rom_available();
            default: return rush2::wings::rom_available();
        }
    }

    // Whether a race can play the song now (the 2049 sound banks load in the background).
    bool playable(int i) {
        const Song& s = songs[i];
        if (!enabled[i].load(std::memory_order_relaxed) || !game_enabled[(int)s.game].load(std::memory_order_relaxed) ||
            !game_available(s.game)) {
            return false;
        }
        return s.game != Game::Rush2049 || rush2::track2049::music_ready();
    }

    int find_song(Game g, int id) {
        for (int i = 0; i < song_count; i++) {
            if (songs[i].game == g && songs[i].id == id) {
                return i;
            }
        }
        return -1;
    }

    // A random playable song (of game g, or any game when all is set), skipping the last one when there's another.
    int pick(bool all, Game g) {
        std::vector<int> pool;
        for (int i = 0; i < song_count; i++) {
            if ((all || songs[i].game == g) && playable(i)) {
                pool.push_back(i);
            }
        }
        if (pool.size() > 1) {
            std::erase(pool, last_song);
        }
        if (pool.empty()) {
            return -1;
        }
        return pool[std::uniform_int_distribution<size_t>(0, pool.size() - 1)(rng)];
    }

    // The race's song (catalog index), or -1 to leave it to the game.
    int choose(uint8_t* rdram) {
        int8_t t = (int8_t)MEM_B(0, (int32_t)track_id);
        bool host = t == rush2::track2049::host_slot;
        Game g = Game::Rush2;
        int original = -1;
        if (host && rush2::track2049::race_track() > 0) {
            g = Game::Rush2049;
            original = first_rush2049 + rush2::track2049::race_track() - 1;
        }
        else if (t == rush2::track2049::stunt_host_slot && rush2::track2049::stunt_arena() > 0) {
            // Rush 2049's per-track songs (0x8010FFD4): 8 for stunt arenas 1 and 2, 9 for 3 and 4.
            g = Game::Rush2049;
            original = find_song(Game::Rush2049, rush2::track2049::stunt_arena() <= 2 ? 8 : 9);
        }
        else if (host && rush2::track1::race_track() > 0) {
            g = Game::Rush1;
            original = pick(false, Game::Rush1);
        }
        else if (t >= 0 && t < 12) {
            original = find_song(Game::Rush2, (int16_t)MEM_H(0, (int32_t)(track_songs + t * 2)));
        }
        else {
            original = pick(false, Game::Rush2);
        }

        int song = -1;
        switch ((Mode)modes[(int)g].load()) {
            case Mode::Original:
                song = original >= 0 && playable(original) ? original : pick(false, g);
                break;
            case Mode::ShuffleByGame:
                song = pick(false, g);
                break;
            case Mode::ShuffleAll:
                song = pick(true, g);
                break;
        }
        if (song < 0) {
            song = pick(true, g);
        }
        return song;
    }

    // Rush 2's sequence number for a Rush 2 or SF Rush song, or -1.
    int sequence_of(uint8_t* rdram, recomp_context* ctx, int i) {
        const Song& s = songs[i];
        if (s.game == Game::Rush2) {
            return (int16_t)MEM_H(0, (int32_t)(song_table + s.id * 2));
        }
        if (s.game == Game::Rush1) {
            return rush2::track1::song_sequence(rdram, ctx, s.id);
        }
        return -1;
    }

    bool rush2_song_playing(uint8_t* rdram, recomp_context* ctx) {
        recomp_context call = *ctx;
        call.r4 = MEM_W(0, (int32_t)sequence_player);
        func_800091B0(rdram, &call);
        return (int32_t)call.r2 == al_playing;
    }

    void stop_rush2_song(uint8_t* rdram, recomp_context* ctx) {
        recomp_context call = *ctx;
        func_80061E10(rdram, &call);
    }

    // Starts sequence `sequence` on Rush 2's player; false if the loader is busy.
    bool play_rush2_song(uint8_t* rdram, recomp_context* ctx, int sequence) {
        recomp_context call = *ctx;
        call.r4 = sequence;
        call.r5 = (int32_t)-1;
        func_80061E68(rdram, &call);
        return (int32_t)call.r2 != 0;
    }

    // Starts catalog song i as the preview. False to try again later (loader busy, 2049 banks still loading).
    bool start_preview(uint8_t* rdram, recomp_context* ctx, int i) {
        if (songs[i].game == Game::Rush2049) {
            if (!rush2::track2049::music_ready()) {
                return false;
            }
            stop_rush2_song(rdram, ctx);
            rush2::track2049::play_song_now(rdram, songs[i].id);
            preview.sequence = -1;
        }
        else {
            int sequence = sequence_of(rdram, ctx, i);
            if (sequence < 0) {
                return true; // Not available: give up.
            }
            rush2::track2049::stop_song_now();
            if (!play_rush2_song(rdram, ctx, sequence)) {
                return false;
            }
            preview.sequence = sequence;
        }
        preview.song = i;
        previewing = i;
        return true;
    }

    // Starts holding the game's music commands: the ones still queued become volume updates (game_music already has
    // them). A Rush 2 song the game asked for that has ended (a jingle) isn't brought back.
    void begin_preview(uint8_t* rdram, recomp_context* ctx) {
        preview.active = true;
        uint16_t read = (uint16_t)MEM_H(0, (int32_t)command_read);
        uint16_t write = (uint16_t)MEM_H(0, (int32_t)command_write);
        bool queued = false;
        for (uint16_t i = read & 15; i != (write & 15); i = (i + 1) & 15) {
            uint32_t entry = command_queue + i * 4;
            if (((uint32_t)MEM_W(0, (int32_t)entry) >> 30) != 3) {
                MEM_W(0, (int32_t)entry) = (int32_t)command_volume;
                queued = true;
            }
        }
        uint8_t loaded = (uint8_t)MEM_B(0, (int32_t)current_song);
        uint8_t loading = (uint8_t)MEM_B(0, (int32_t)(current_song + 1));
        if (!queued && game_music.sequence >= 0 && game_music.sequence == loaded && loading == loaded &&
            !rush2_song_playing(rdram, ctx)) {
            game_music.sequence = -1;
        }
    }

    // Ends the preview and plays what the game last asked for. False to try again (loader busy).
    bool end_preview(uint8_t* rdram, recomp_context* ctx) {
        if (game_music.song_2049 >= 0) {
            stop_rush2_song(rdram, ctx);
            if (rush2::track2049::playing_song() != game_music.song_2049) {
                rush2::track2049::play_song_now(rdram, game_music.song_2049);
            }
        }
        else if (game_music.sequence >= 0) {
            rush2::track2049::stop_song_now();
            if (!play_rush2_song(rdram, ctx, game_music.sequence)) {
                return false;
            }
        }
        else {
            rush2::track2049::stop_song_now();
            stop_rush2_song(rdram, ctx);
        }
        preview = PreviewState{};
        previewing = -1;
        return true;
    }

    // Song list rows on the Sound tab.
    const std::string description =
        "Each game's <recomp-color primary>Race Music</recomp-color> sets how a race on that game's tracks picks its "
        "song when the game's own MUSIC setting is on its default (a song per track). A fixed song or Off in the "
        "game's menu still wins.\n\n"
        "<recomp-color primary>Original</recomp-color> plays the song the track's game gives it (SF Rush picks one of "
        "its songs at random). When that song is switched off below, the track plays another song from its game.\n\n"
        "<recomp-color primary>Shuffle</recomp-color> plays a random song from the track's own game.\n\n"
        "<recomp-color primary>Shuffle All</recomp-color> plays a random song from all three games.\n\n"
        "Switch songs off below to keep them out of races, or a whole game with the switch by its name (its songs "
        "keep their own switches for when it's back on). Press a song's play button to hear it. SF Rush and "
        "Rush 2049 songs need their ROMs (SF Rush and Rush 2049 tabs).";

    // A row of the page: scrolls itself into view when something in it takes focus, so the list follows the d-pad.
    class PageRow : public Element {
    public:
        PageRow(ResourceId rid, Element* parent) : Element(rid, parent, Events(EventType::Focus), "div", false) {
            set_width(100.0f, Unit::Percent);
            set_as_navigation_container(NavigationType::Auto);
        }

    protected:
        std::string_view get_type_name() override { return "SoundPageRow"; }

        void process_event(const Event& e) override {
            if (e.type == EventType::Focus && std::get<EventFocus>(e.variant).active) {
                scroll_into_view();
            }
        }
    };

    // The play/stop button: one element whose icon changes, so it keeps focus when pressed.
    class PreviewButton : public IconButton {
    public:
        using IconButton::IconButton;

        void show_playing(bool playing) {
            svg->set_src(playing ? "icons/Stop.svg" : "icons/Play.svg");
            apply_button_style(playing ? ButtonStyle::Primary : ButtonStyle::Secondary);
        }
    };

    class SoundPage : public ConfigPage {
    public:
        SoundPage(ResourceId rid, Element* parent) : ConfigPage(rid, parent, Events(EventType::Update)) {
            ContextId context = get_current_context();
            set_as_navigation_container(NavigationType::Vertical);

            Element* left = body->get_left();
            left->set_padding(0.0f);
            left->set_display(Display::Block);
            left->set_position(Position::Relative);
            left->set_height(100.0f, Unit::Percent);
            list = context.create_element<Element>(left, 0, "div", false);
            list->set_display(Display::Block);
            list->set_width(100.0f, Unit::Percent);
            list->set_min_height(100.0f, Unit::Percent);
            list->set_max_height(100.0f, Unit::Percent);
            list->set_padding(16.0f);
            list->set_overflow_y(Overflow::Auto);
            list->set_as_navigation_container(NavigationType::Vertical);

            Element* text = context.create_element<Element>(body->get_right(), 0, "p", true);
            text->set_typography(theme::Typography::Body);
            text->set_line_height(28.0f);
            text->set_padding(8.0f);
            std::string html;
            for (char c : description) {
                html += c == '\n' ? std::string("<br/>") : std::string(1, c);
            }
            text->set_text_unsafe(html);

            build();
            queue_update();
        }

    protected:
        std::string_view get_type_name() override { return "SoundPage"; }

        void process_event(const Event& e) override {
            if (e.type == EventType::Update) {
                int now = previewing.load();
                if (now != shown_preview) {
                    show_preview(now);
                }
                // A game switched off, or a ROM chosen on another tab: update the rows in place (rebuilding would
                // delete a focused element).
                for (Game g : games) {
                    if (game_state(g) != shown_state[(int)g]) {
                        refresh_game(g);
                    }
                }
                queue_update();
            }
        }

    private:
        struct Row {
            PreviewButton* button;
            Toggle* toggle;
            Label* name;
        };
        struct Header {
            Toggle* toggle = nullptr;
            Radio* modes = nullptr;
            Label* note = nullptr;
        };
        Element* list = nullptr;
        std::vector<Row> rows;
        Header headers[3];
        int shown_preview = -1;
        int shown_state[3] = { -1, -1, -1 };

        static int game_state(Game g) {
            return (game_available(g) ? 1 : 0) | (game_enabled[(int)g].load() ? 2 : 0);
        }

        void show_preview(int now) {
            for (int i = 0; i < (int)rows.size(); i++) {
                if (i == now || i == shown_preview) {
                    rows[i].button->show_playing(i == now);
                }
            }
            shown_preview = now;
        }

        // A game's rows: previews need its ROM; its song switches also need the game's switch on.
        void refresh_game(Game g) {
            int state = game_state(g);
            bool available = (state & 1) != 0;
            bool on = (state & 2) != 0;
            for (int i = 0; i < (int)rows.size(); i++) {
                if (songs[i].game == g) {
                    rows[i].button->set_enabled(available);
                    rows[i].toggle->set_enabled(available && on);
                    rows[i].name->set_color(available && on ? theme::color::Text : theme::color::TextInactive);
                }
            }
            Header& h = headers[(int)g];
            h.toggle->set_enabled(available);
            h.modes->set_enabled(available);
            if (h.note != nullptr) {
                h.note->set_display(available ? Display::None : Display::Block);
            }
            shown_state[(int)g] = state;
        }

        Element* add_setting(const std::string& name) {
            ContextId context = get_current_context();
            Element* row = context.create_element<PageRow>(list);
            row->set_display(Display::Flex);
            row->set_flex_direction(FlexDirection::Column);
            row->set_padding(12.0f);
            row->set_gap(8.0f);
            context.create_element<Label>(row, name, theme::Typography::LabelMD);
            return row;
        }

        void build() {
            ContextId context = get_current_context();
            recomp::config::Config& config = recompui::config::get_sound_config();

            Element* volume_row = add_setting("Main Volume");
            Slider* volume = context.create_element<Slider>(volume_row, SliderType::Percent);
            const auto& volume_option = std::get<recomp::config::ConfigOptionNumber>(
                config.get_option(recompui::config::sound::options::main_volume).variant);
            volume->set_min_value(volume_option.min);
            volume->set_max_value(volume_option.max);
            volume->set_step_value(volume_option.step);
            volume->set_precision(volume_option.precision);
            volume->set_value(recompui::config::sound::get_main_volume());
            volume->add_value_changed_callback([](double value) {
                recompui::config::get_sound_config().set_option_value(recompui::config::sound::options::main_volume, value);
            });

            // Other cars' engines: a switch on the setting's row, lined up with the song switches.
            Element* engines_row = context.create_element<PageRow>(list);
            engines_row->set_display(Display::Flex);
            engines_row->set_flex_direction(FlexDirection::Row);
            engines_row->set_align_items(AlignItems::Center);
            engines_row->set_padding(12.0f);
            Element* engines_title = context.create_element<Element>(engines_row, 0, "div", false);
            engines_title->set_display(Display::Flex);
            engines_title->set_flex_direction(FlexDirection::Column);
            engines_title->set_flex_grow(1.0f);
            context.create_element<Label>(engines_title, "Other Cars' Engines", theme::Typography::LabelMD);
            Label* engines_note = context.create_element<Label>(engines_title, other_engines_description, theme::Typography::Body);
            engines_note->set_color(theme::color::TextDim);
            Toggle* engines = context.create_element<Toggle>(engines_row, ToggleSize::Medium);
            engines->set_checked(std::get<bool>(config.get_option_value(other_engines_id)));
            engines->add_checked_callback([](bool checked) {
                recompui::config::get_sound_config().set_option_value(other_engines_id, checked);
            });

            for (int i = 0; i < song_count; i++) {
                const Song& s = songs[i];
                if (i == 0 || songs[i - 1].game != s.game) {
                    // The game's header, with its switch lined up with the song switches.
                    Element* header = context.create_element<PageRow>(list);
                    header->set_display(Display::Flex);
                    header->set_flex_direction(FlexDirection::Row);
                    header->set_align_items(AlignItems::Center);
                    header->set_padding_left(12.0f);
                    header->set_padding_right(12.0f);
                    header->set_padding_top(24.0f);
                    header->set_padding_bottom(4.0f);
                    Element* title = context.create_element<Element>(header, 0, "div", false);
                    title->set_display(Display::Flex);
                    title->set_flex_direction(FlexDirection::Column);
                    title->set_flex_grow(1.0f);
                    context.create_element<Label>(title, std::string(game_name(s.game)) + " Songs", theme::Typography::LabelLG);
                    Header& h = headers[(int)s.game];
                    if (s.game != Game::Rush2) {
                        h.note = context.create_element<Label>(title,
                            s.game == Game::Rush1 ? "Needs a San Francisco Rush (USA) ROM (SF Rush tab)."
                                                  : "Needs a Rush 2049 (USA) ROM (Rush 2049 tab).",
                            theme::Typography::Body);
                        h.note->set_color(theme::color::TextDim);
                    }
                    Game g = s.game;
                    h.toggle = context.create_element<Toggle>(header, ToggleSize::Medium);
                    h.toggle->set_checked(std::get<bool>(config.get_option_value(game_option_id(g))));
                    h.toggle->add_checked_callback([g](bool checked) {
                        recompui::config::get_sound_config().set_option_value(game_option_id(g), checked);
                    });

                    Element* mode_row = context.create_element<PageRow>(list);
                    mode_row->set_display(Display::Flex);
                    mode_row->set_flex_direction(FlexDirection::Row);
                    mode_row->set_align_items(AlignItems::Center);
                    mode_row->set_gap(16.0f);
                    mode_row->set_padding_left(12.0f);
                    mode_row->set_padding_right(12.0f);
                    mode_row->set_padding_bottom(8.0f);
                    Label* mode_label = context.create_element<Label>(mode_row, "Race Music", theme::Typography::LabelSM);
                    mode_label->set_color(theme::color::TextDim);
                    h.modes = context.create_element<Radio>(mode_row);
                    std::vector<Mode> choices = game_modes(g);
                    uint32_t current = std::get<uint32_t>(config.get_option_value(mode_option_id(g)));
                    for (size_t k = 0; k < choices.size(); k++) {
                        h.modes->add_option(mode_name(choices[k]));
                        if ((uint32_t)choices[k] == current) {
                            h.modes->set_index((uint32_t)k);
                        }
                    }
                    h.modes->add_index_changed_callback([g](uint32_t index) {
                        std::vector<Mode> choices = game_modes(g);
                        if (index < choices.size()) {
                            recompui::config::get_sound_config().set_option_value(mode_option_id(g), (uint32_t)choices[index]);
                        }
                    });
                }

                Element* row = context.create_element<PageRow>(list);
                row->set_display(Display::Flex);
                row->set_flex_direction(FlexDirection::Row);
                row->set_align_items(AlignItems::Center);
                row->set_gap(16.0f);
                row->set_padding_left(12.0f);
                row->set_padding_right(12.0f);
                row->set_padding_top(6.0f);
                row->set_padding_bottom(6.0f);

                PreviewButton* button = context.create_element<PreviewButton>(row, "icons/Play.svg", ButtonStyle::Secondary, IconButtonSize::Medium);
                button->add_pressed_callback([i]() {
                    preview_request = previewing.load() == i ? stop_request : i;
                });

                Element* text = context.create_element<Element>(row, 0, "div", false);
                text->set_display(Display::Flex);
                text->set_flex_direction(FlexDirection::Column);
                text->set_flex_grow(1.0f);
                text->set_gap(2.0f);
                Label* name = context.create_element<Label>(text, s.name, theme::Typography::LabelMD);
                Label* where = context.create_element<Label>(text, s.plays_on, theme::Typography::LabelSM);
                where->set_color(theme::color::TextDim);

                Toggle* toggle = context.create_element<Toggle>(row, ToggleSize::Medium);
                toggle->set_checked(std::get<bool>(config.get_option_value(song_option_id(s))));
                toggle->add_checked_callback([i](bool checked) {
                    recompui::config::get_sound_config().set_option_value(song_option_id(songs[i]), checked);
                });
                rows.push_back({ button, toggle, name });
            }
            for (Game g : games) {
                refresh_game(g);
            }
            show_preview(previewing.load());
        }
    };
}

void rush2::music::create_sound_tab() {
    recomp::config::Config& config = recompui::config::create_sound_tab();
    config.add_bool_option(other_engines_id, "Other Cars' Engines", other_engines_description, true, true);
    config.add_option_change_callback(other_engines_id,
        [](recomp::config::ConfigValueVariant cur_value, recomp::config::ConfigValueVariant, recomp::config::OptionChangeContext) {
            rush2::car_engines::set_enabled(std::get<bool>(cur_value));
        });
    for (Game g : games) {
        std::vector<recomp::config::ConfigOptionEnumOption> choices;
        for (Mode m : game_modes(g)) {
            const char* key = m == Mode::Original ? "Original" : m == Mode::ShuffleByGame ? "ShuffleByGame" : "ShuffleAll";
            choices.push_back({ m, key, mode_name(m) });
        }
        modes[(int)g] = (uint32_t)Mode::Original;
        config.add_enum_option(mode_option_id(g), std::string(game_name(g)) + " Race Music", description, choices,
            Mode::Original, true);
        config.add_option_change_callback(mode_option_id(g),
            [g](recomp::config::ConfigValueVariant cur_value, recomp::config::ConfigValueVariant, recomp::config::OptionChangeContext) {
                modes[(int)g] = std::get<uint32_t>(cur_value);
            });
    }
    for (Game g : games) {
        game_enabled[(int)g] = true;
        config.add_bool_option(game_option_id(g), std::string(game_name(g)) + " Songs",
            "Lets races play this game's songs.", true, true);
        config.add_option_change_callback(game_option_id(g),
            [g](recomp::config::ConfigValueVariant cur_value, recomp::config::ConfigValueVariant, recomp::config::OptionChangeContext) {
                game_enabled[(int)g] = std::get<bool>(cur_value);
            });
    }
    for (int i = 0; i < song_count; i++) {
        const Song& s = songs[i];
        enabled[i] = true;
        config.add_bool_option(song_option_id(s), std::string(game_name(s.game)) + ": " + s.name,
            "Lets races play this song.", true, true);
        config.add_option_change_callback(song_option_id(s),
            [i](recomp::config::ConfigValueVariant cur_value, recomp::config::ConfigValueVariant, recomp::config::OptionChangeContext) {
                enabled[i] = std::get<bool>(cur_value);
            });
    }

    // The frontend's Sound tab is replaced by one with the song list; its config (sound.json) is kept.
    recompui::config::set_tab_visible(recompui::config::sound::id, false);
    recompui::config::create_tab(recompui::config::sound::tab_name, tab_id,
        [](ContextId context, Element* parent) {
            context.create_element<SoundPage>(parent);
        },
        nullptr,
        [](TabCloseContext) {
            preview_request = stop_request;
            recompui::config::get_sound_config().save_config();
        });
}

void rush2::music::load_config() {
    recomp::config::Config& config = recompui::config::get_sound_config();
    rush2::car_engines::set_enabled(std::get<bool>(config.get_option_value(other_engines_id)));
    for (Game g : games) {
        modes[(int)g] = std::get<uint32_t>(config.get_option_value(mode_option_id(g)));
    }
    for (int i = 0; i < song_count; i++) {
        enabled[i] = std::get<bool>(config.get_option_value(song_option_id(songs[i])));
    }
    for (Game g : games) {
        game_enabled[(int)g] = std::get<bool>(config.get_option_value(game_option_id(g)));
    }
    // Start loading Rush 2049's sound banks now, so its songs are ready for the first race.
    if (rush2::wings::rom_available()) {
        rush2::track2049::music_ready();
    }
}

// func_8008C370 entry: $a0 = 2 starts the race's music with setting $a1.
extern "C" void rush2_music_race(uint8_t* rdram, recomp_context* ctx) {
    race_sequence = -1;
    rush2::track2049::queue_race_song(-1);
    if ((int32_t)ctx->r4 != 2 || (ctx->r5 & 0xFF) != 1) {
        return;
    }
    std::lock_guard lock{ choice_mutex };
    int song = choose(rdram);
    if (song < 0) {
        return;
    }
    const Song& s = songs[song];
    if (s.game == Game::Rush2049) {
        rush2::track2049::queue_race_song(s.id);
        ctx->r5 = 0;
    }
    else {
        int sequence = sequence_of(rdram, ctx, song);
        if (sequence < 0) {
            return;
        }
        race_sequence = sequence;
    }
    last_song = song;
    fprintf(stderr, "[Music] Race song: %s %s\n", game_name(s.game), s.name);
}

// func_8008C370 at 0x8008C46C: $v0 = the sequence Rush 2 chose for the race.
extern "C" void rush2_music_race_sequence(uint8_t* rdram, recomp_context* ctx) {
    int sequence = race_sequence.exchange(-1);
    if (sequence >= 0) {
        ctx->r2 = sequence;
    }
}

bool rush2::music::game_command(uint8_t* rdram, recomp_context* ctx) {
    uint32_t cmd = (uint32_t)ctx->r4;
    uint32_t kind = cmd >> 30;
    if (kind == 3) {
        return false; // Volume and fade.
    }
    std::lock_guard lock{ preview_mutex };
    if (kind == 0) {
        game_music = { (int)(cmd >> 16), -1 };
    }
    else if (kind == 2) {
        game_music = { (int)(cmd & 0x7FFF), -1 };
    }
    else {
        game_music = { -1, rush2::track2049::queued_race_song() };
    }
    if (!preview.active) {
        return false;
    }
    if (kind == 1) {
        rush2::track2049::queue_race_song(-1);
    }
    ctx->r4 = (int32_t)command_volume;
    return true;
}

// func_800631A4 (the audio thread) at 0x80063AA0, before it takes the queued music commands: runs preview requests.
extern "C" void rush2_music_preview(uint8_t* rdram, recomp_context* ctx) {
    int request = preview_request.load();
    if (request == no_request) {
        return;
    }
    std::lock_guard lock{ preview_mutex };
    if (request == stop_request) {
        if (!preview.active || end_preview(rdram, ctx)) {
            preview_request.compare_exchange_strong(request, no_request);
        }
        return;
    }
    if (request < 0 || request >= song_count) {
        preview_request.compare_exchange_strong(request, no_request);
        return;
    }
    if (!preview.active) {
        begin_preview(rdram, ctx);
    }
    if (start_preview(rdram, ctx, request)) {
        preview_request.compare_exchange_strong(request, no_request);
    }
}
