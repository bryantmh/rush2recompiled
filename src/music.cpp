// Race music across Rush 2, SF Rush, Rush 2049 and California Speed: the Sound tab's song list, how a race picks its
// song, and song previews.
//
// Songs: the ones the games play on their race tracks. Rush 2's eight (its MUSIC setting's 2-9, sequences
// 0x800CC394 = [3, 4, 0, 1, 7, 10, 2, 8]; per-track choice 0x800CC37C), SF Rush's nine race songs (its random table
// 0x800D2910, played through Rush 2's player as sequences 13-28, src/rush1/track1_audio.cpp) and the six songs of Rush
// 2049's race tracks (0x8010FFD4, played on the host, src/rush2049/track2049_audio.cpp). A Rush 2049 Dreamcast disc has its own
// 20 songs (rush2::audio2049::disc_songs, a song per track at 0x8C0BA76C). California Speed's nine songs (its song
// file, ROM 0xD69FC0) play through Rush 2's player as sequences 29-37 (src/rush1/track1_audio.cpp). California Speed
// has no per-track songs (the player picks one: its options' Audio Track), so like SF Rush its tracks play a random one
// of its songs by default. Streamed songs (src/music_streams.cpp, played on the host): the Rush 2049 arcade's (a song
// per arcade track, listed under Rush 2049), the PlayStation SF Rush disc's CD audio (four songs that, like SF Rush's
// N64 ones, play on any SF Rush track) and the custom tracks' own song files (the Custom Tracks column).
//
// Sources: Rush 2049's songs come from its N64 ROM, Dreamcast disc and arcade data, SF Rush's from its N64 ROM,
// PlayStation disc and arcade data. Each source installed is listed under its heading, and with more than one
// installed each has a switch (source_switches; they replaced the N64 / Dreamcast / Both Soundtrack choice, migrated
// at startup by src/data_files.cpp). The other Rush 2049 source's songs load beside the first's while its switch is
// on (audio2049::load_music).
//
// Choice: Rush 2 picks a race's song in func_8008C370(2, setting), setting 1 being "per track" (byte 0x800D5775).
// Only that setting is taken over; the fixed-song settings and Off stay the game's. Each game's Race Music option
// chooses for that game's tracks: Original, Shuffle (a random song of the game) or Shuffle All (a random song of any
// game in Shuffle All). SF Rush and California Speed offer only Original and Shuffle All: their own choice already is
// a random song of the game. A game's In Shuffle All switch keeps its songs out of every Shuffle All (its own tracks'
// Original and Shuffle still play them).
// Original plays the song of the version raced (race_music): a course's versions are the track select's
// (track2049::course_versions: the N64 version, the Dreamcast one, the custom tracks that are versions of it, such as
// the arcade's), and each version has its own song: the N64 ROM's or the disc's for a Rush 2049 race track, the
// arcade's for an arcade version, a custom track's [audio] music, Rush 2's for its tracks. A version with no song of
// its own (an SF Rush or California Speed track, which pick at random; a custom version without music) plays the
// course's, or the game's random pick (SF Rush's from its N64 and PlayStation songs). Switched off, the next version's
// song plays, then another song of the game. A custom track that is no other track's version counts as a Rush 2
// track; one with no song of its own has as its Original a random song of any game, of every song switched on.
// SKIP TRACK goes through the course's versions' songs first (the next after the one playing, each once a race),
// then picks by the game's Race Music. Shuffles skip the song played last. A song of a game whose ROM or audio isn't available is skipped; with nothing
// left the game picks as it would by itself. A Rush 2 or SF Rush song replaces the sequence Rush 2 chose (at
// 0x8008C46C); a Rush 2049 song is queued on the 2049 player and the call turned into the "music off" case, whose
// stop command starts it (src/rush2049/track2049_audio.cpp).
//
// Menu: each game's main menu song has a switch (Rush 2049 three: the N64's, the Dreamcast's and the arcade's select
// screen song); each time the front end's song starts (command 0x0009FFFF) a random
// one of those switched on plays (not the last one, when there's another), Rush 2's when none is on or can play.
// Older versions' single Main Menu Music choice (sound.json "menu_music") becomes the switches at startup
// (src/data_files.cpp).
//
// Preview: the Sound tab's play buttons. The game keeps running while the menu is open, so the preview runs on its
// audio thread (func_800631A4, before it takes queued music commands): Rush 2 and SF Rush songs start with
// func_80061E68 (0 while it is still stopping or loading the old song, so the request waits; 2 if that song already
// plays), Rush 2049 songs on the host player, each stopping the other. Every music command the game queues
// (func_80062F50) is noted as the music the game wants; while a preview plays, its plays and stops are held (turned
// into a volume update, as are any still in the queue when the preview starts), so the game can't take the music
// back. Stopping the preview (or closing the tab) then plays what the game last asked for.
//
// Pause: race_pause_menu_step (func_800AFD44) stops the music when the race pauses (music command 0x40000000 at
// 0x800AFE40, after func_80062FC4 at 0x800AFE38) and on CONTINUE starts the race's song from the top again with
// func_8008C370(2, MUSIC setting) at 0x800B00B8 (with a shuffle, a new song). The Keep Music Playing When Paused
// option (off by default) holds both: the stop becomes a volume update and the restart does nothing, so the song
// plays on through the pause. The option can be changed with the pause menu open (the Sound tab works in game): the
// pause step's entry hook then starts the race's song at once (func_8008C370, as CONTINUE would) or stops it (music
// command 0x40000000, as the pause does), and CONTINUE restarts the song only if it isn't playing.

// As in src/rush2049/wings.cpp: librecomp's nlohmann::json first.
#include "../lib/N64ModernRuntime/thirdparty/json/json.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <thread>
#include <cstdio>
#include <mutex>
#include <random>
#include <string>
#include <vector>

#include "recompui/config.h"
#include "sfx_sweep.h"
#include "librecomp/config.hpp"
#include "elements/ui_config_page.h"
#include "elements/ui_icon_button.h"
#include "elements/ui_label.h"
#include "elements/ui_select.h"
#include "elements/ui_slider.h"
#include "elements/ui_toggle.h"

#include "options_page.h"
#include "rush2_hooks.h"
#include "music.h"
#include "track1.h"
#include "track2049.h"
#include "car_engines.h"
#include "announcer.h"
#include "audio2049.h"
#include "wings.h"
#include "calspeed.h"
#include "music_streams.h"
#include "track_mod_runtime.h"
#include "arcade2049.h"
#include "sfrush_ps1.h"

extern "C" void music_stop_song_80061E10(uint8_t* rdram, recomp_context* ctx);   // Stops the song.
extern "C" void music_play_sequence_80061E68(uint8_t* rdram, recomp_context* ctx);   // Plays sequence $a0; 0 if busy.
extern "C" void al_seqp_get_state_800091B0(uint8_t* rdram, recomp_context* ctx);   // alSeqpGetState($a0)
extern "C" void music_pick_race_song_8008C370(uint8_t* rdram, recomp_context* ctx);   // ($a0 2: race music, $a1 setting)
extern "C" void music_queue_command_80062F50(uint8_t* rdram, recomp_context* ctx);  // Queues music command $a0.

namespace {
    using namespace recompui;

    constexpr uint32_t track_id = 0x8010C3F0;
    // The other cars' engine sounds (src/car_engines.cpp).
    constexpr const char* other_engines_id = "other_engines";
    constexpr const char* pause_music_id = "pause_keeps_music";
    std::atomic<bool> pause_keeps_music = false;
    // Set around the pause menu's stop and restart calls while the option is on: their music commands are held.
    std::atomic<bool> pause_hold = false;
    // During a pause: 1 if the race's song plays on, 0 if it was stopped; -1 outside a pause.
    std::atomic<int> pause_music = -1;
    constexpr const char* menu_song_description =
        "Plays this game's main menu song in the menus. With several switched on, each time the menu music starts "
        "it picks one of them. With none on, Rush 2's plays.";
    constexpr const char* pause_music_description =
        "Keeps the race's song playing in the pause menu. Off, as in Rush 2, pausing stops it and continuing starts "
        "it over.";
    constexpr const char* other_engines_description =
        "Hear the computer cars' engines around you, as Rush 2049 does (Rush 2 plays only your own).";
    // The race start announcer (src/announcer.cpp).
    constexpr const char* announcer_id = "race_announcer";
    constexpr const char* announcer_description =
        "Which game's announcer calls the countdown and GO at the start of a race. SF Rush and Rush 2049 use Rush 2's. "
        "California Speed needs its ROM; without it Rush 2's plays.";
    constexpr uint32_t track_songs = 0x800CC37C;    // s16[12]: index into the song table per track.
    constexpr uint32_t song_table = 0x800CC394;     // s16[8]: sequence per Rush 2 race song.
    constexpr uint32_t requested_song = 0x800F9550; // Sequence func_80061E68 last queued for the loader.
    constexpr uint32_t sequence_player = 0x800FAEB4; // ALSeqPlayer*
    constexpr int al_playing = 1;

    // Values are kept in the configs (Main Menu Music stores Rush2-Rush2049 as 0-2, then 3 for Shuffle). Mods: the
    // custom tracks' own song files (src/music_streams.cpp).
    enum class Game { Rush2, Rush1, Rush2049, CalSpeed, Mods };
    // Where a song comes from, for a game with more than one source: Rush 2049's N64 ROM, Dreamcast disc or arcade;
    // SF Rush's N64 ROM, PlayStation disc or arcade (Any: the game's only source, SF Rush's N64 songs among them).
    enum class Only { Any, N64, Disc, Arcade, PS1, Mod };

    struct Song {
        Game game;
        // Rush 2: song table index. SF Rush and California Speed: the game's song number. Rush 2049 and the streamed
        // songs: the host player's song id (audio2049: N64 0-11, disc_songs + n, stream_songs and up).
        int id;
        std::string key;    // Option id suffix.
        std::string name;
        std::string plays_on;
        Only only = Only::Any;
    };

    constexpr int disc = rush2::audio2049::disc_songs;
    constexpr int arc49 = rush2::audio2049::arcade2049_songs;
    constexpr int ps1 = rush2::audio2049::ps1_songs;
    constexpr int rock = rush2::audio2049::rock_songs;

    // In each game's own order: Rush 2's MUSIC setting, SF Rush's random table (then the PlayStation disc's CD audio),
    // Rush 2049's tracks (the N64's, then the disc's by its track table 0x8C0BA76C: races, battles, stunts, obstacle
    // course, then the songs no track plays, then the arcade's by its TRACKn). The N64 ROM names no songs, so each N64
    // song is named after the first track that plays it (an earlier tempo/chroma match against the disc's songs was
    // too weak to trust: docs rush2049_research/audio.md "Songs"); the arcade's are named after their tracks.
    const std::array<Song, 73> songs = {{
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
        { Game::Rush1, ps1 + 0, "r1ps1_0", "Song 1", "Any SF Rush track", Only::PS1 },
        { Game::Rush1, ps1 + 1, "r1ps1_1", "Song 2", "Any SF Rush track", Only::PS1 },
        { Game::Rush1, ps1 + 2, "r1ps1_2", "Song 3", "Any SF Rush track", Only::PS1 },
        { Game::Rush1, ps1 + 3, "r1ps1_3", "Song 4", "Any SF Rush track", Only::PS1 },
        { Game::Rush1, rock + 0, "r1arc_27", "Golden Gate", "SF Rush Golden Gate Arcade", Only::Arcade },
        { Game::Rush1, rock + 1, "r1arc_25", "Embarcadero", "SF Rush Embarcadero Arcade", Only::Arcade },
        { Game::Rush1, rock + 2, "r1arc_31", "Market", "SF Rush Market Arcade", Only::Arcade },
        { Game::Rush1, rock + 4, "r1arc_29", "Downtown", "SF Rush Downtown Arcade", Only::Arcade },
        { Game::Rush1, rock + 5, "r1arc_33", "The Heights", "SF Rush The Heights Arcade", Only::Arcade },
        { Game::Rush1, rock + 6, "r1arc_35", "Sunset", "SF Rush Sunset Arcade", Only::Arcade },
        { Game::Rush1, rock + 3, "r1arc_41", "The Rock", "SF Rush The Rock Arcade", Only::Arcade },
        { Game::Rush2049, 0, "r49_0", "Marina", "Rush 2049 Marina, Battle 1", Only::N64 },
        { Game::Rush2049, 1, "r49_1", "Haight", "Rush 2049 Haight, Battle 2", Only::N64 },
        { Game::Rush2049, 4, "r49_4", "Civic", "Rush 2049 Civic, Battle 3", Only::N64 },
        { Game::Rush2049, 2, "r49_2", "Metro", "Rush 2049 Metro, Battle 4", Only::N64 },
        { Game::Rush2049, 3, "r49_3", "Mission", "Rush 2049 Mission, Battle 5", Only::N64 },
        { Game::Rush2049, 7, "r49_7", "Presidio", "Rush 2049 Presidio, Battle 6", Only::N64 },
        { Game::Rush2049, 8, "r49_8", "Stunt 1", "Rush 2049 Stunt 1, 2, Obstacle, Battle 7", Only::N64 },
        { Game::Rush2049, 9, "r49_9", "Stunt 3", "Rush 2049 Stunt 3, 4, Battle 8", Only::N64 },
        { Game::Rush2049, disc + 5, "r49dc_5", "Morning", "Rush 2049 Marina", Only::Disc },
        { Game::Rush2049, disc + 7, "r49dc_7", "Noon", "Rush 2049 Haight", Only::Disc },
        { Game::Rush2049, disc + 12, "r49dc_12", "Sunset", "Rush 2049 Civic", Only::Disc },
        { Game::Rush2049, disc + 6, "r49dc_6", "Night", "Rush 2049 Metro", Only::Disc },
        { Game::Rush2049, disc + 2, "r49dc_2", "Garage", "Rush 2049 Mission, Battle 6", Only::Disc },
        { Game::Rush2049, disc + 13, "r49dc_13", "The Rock", "Rush 2049 Presidio", Only::Disc },
        { Game::Rush2049, disc + 4, "r49dc_4", "High", "Rush 2049 Battle 1, 8", Only::Disc },
        { Game::Rush2049, disc + 15, "r49dc_15", "Vice", "Rush 2049 Battle 2", Only::Disc },
        { Game::Rush2049, disc + 10, "r49dc_10", "Starsky", "Rush 2049 Battle 3", Only::Disc },
        { Game::Rush2049, disc + 8, "r49dc_8", "Robo", "Rush 2049 Battle 4", Only::Disc },
        { Game::Rush2049, disc + 0, "r49dc_0", "Bassy", "Rush 2049 Battle 5", Only::Disc },
        { Game::Rush2049, disc + 16, "r49dc_16", "Warrior", "Rush 2049 Battle 7", Only::Disc },
        { Game::Rush2049, disc + 11, "r49dc_11", "Stunted", "Rush 2049 Stunt 1", Only::Disc },
        { Game::Rush2049, disc + 1, "r49dc_1", "Flier", "Rush 2049 Stunt 2", Only::Disc },
        { Game::Rush2049, disc + 17, "r49dc_17", "Wingey", "Rush 2049 Stunt 3", Only::Disc },
        { Game::Rush2049, disc + 14, "r49dc_14", "Trancey", "Rush 2049 Stunt 4", Only::Disc },
        { Game::Rush2049, disc + 3, "r49dc_3", "Hidden", "Rush 2049 Obstacle", Only::Disc },
        { Game::Rush2049, disc + 9, "r49dc_9", "Speed", "No Rush 2049 track", Only::Disc },
        { Game::Rush2049, disc + 19, "r49dc_19", "Select", "Rush 2049 menus", Only::Disc },
        { Game::Rush2049, disc + 18, "r49dc_18", "High Score", "Rush 2049 high score entry", Only::Disc },
        { Game::Rush2049, arc49 + 0, "r49arc_1", "Marina", "Rush 2049 Marina Arcade", Only::Arcade },
        { Game::Rush2049, arc49 + 1, "r49arc_2", "Haight", "Rush 2049 Haight Arcade", Only::Arcade },
        { Game::Rush2049, arc49 + 2, "r49arc_3", "Civic", "Rush 2049 Civic Arcade", Only::Arcade },
        { Game::Rush2049, arc49 + 4, "r49arc_5", "Metro", "Rush 2049 Metro Arcade", Only::Arcade },
        { Game::Rush2049, arc49 + 5, "r49arc_6", "Mission", "Rush 2049 Mission Arcade", Only::Arcade },
        { Game::Rush2049, arc49 + 6, "r49arc_7", "Presidio", "Rush 2049 Presidio Arcade", Only::Arcade },
        { Game::Rush2049, arc49 + 3, "r49arc_4", "Alcatraz", "Rush 2049 Alcatraz", Only::Arcade },
        { Game::Rush2049, arc49 + rush2::music_streams::arcade_track8, "r49arc_8", "The Rock",
          "No track here (the arcade's TRACK8)", Only::Arcade },
        { Game::CalSpeed, 0, "cs_0", "Song 1", "Any California Speed track, and its menus" },
        { Game::CalSpeed, 1, "cs_1", "Song 2", "Any California Speed track" },
        { Game::CalSpeed, 2, "cs_2", "Song 3", "Any California Speed track" },
        { Game::CalSpeed, 3, "cs_3", "Song 4", "Any California Speed track" },
        { Game::CalSpeed, 4, "cs_4", "Song 5", "Any California Speed track" },
        { Game::CalSpeed, 5, "cs_5", "Song 6", "Any California Speed track" },
        { Game::CalSpeed, 6, "cs_6", "Song 7", "Any California Speed track" },
        { Game::CalSpeed, 7, "cs_7", "Song 8", "Any California Speed track" },
        { Game::CalSpeed, 8, "cs_8", "Song 9", "Any California Speed track" },
    }};
    constexpr int song_count = (int)std::tuple_size_v<decltype(songs)>;

    // The custom tracks' song files (src/music_streams.cpp) follow the built-in songs as catalog indices
    // mod_base + n, n into the snapshot below; they have no switches of their own (the Mods column's switch).
    constexpr int mod_base = 1000;
    std::mutex mod_catalog_mutex;
    std::shared_ptr<const std::vector<Song>> mod_catalog = std::make_shared<std::vector<Song>>();

    std::shared_ptr<const std::vector<Song>> mod_snapshot() {
        std::lock_guard lock{ mod_catalog_mutex };
        return mod_catalog;
    }

    // Reads the enabled mods' song files again (cheap while they haven't changed).
    void refresh_mod_catalog() {
        auto list = std::make_shared<std::vector<Song>>();
        for (const rush2::music_streams::ModSong& m : rush2::music_streams::mod_songs()) {
            list->push_back({ Game::Mods, m.id, m.owner + "/" + m.name, m.name, m.track, Only::Mod });
        }
        std::lock_guard lock{ mod_catalog_mutex };
        mod_catalog = std::move(list);
    }

    // Catalog index i's song (a built-in one, or a mod's), or nullptr.
    const Song* song_at(int i, const std::vector<Song>& mods) {
        if (i >= 0 && i < song_count) return &songs[(size_t)i];
        if (i >= mod_base && i - mod_base < (int)mods.size()) return &mods[(size_t)(i - mod_base)];
        return nullptr;
    }

    // Every catalog index: the built-in songs, then the mods'.
    std::vector<int> catalog(const std::vector<Song>& mods) {
        std::vector<int> out;
        for (int i = 0; i < song_count; i++) out.push_back(i);
        for (int i = 0; i < (int)mods.size(); i++) out.push_back(mod_base + i);
        return out;
    }

    // The catalog index of a mod song by its song id, or -1.
    int find_mod_song(const std::vector<Song>& mods, int id) {
        for (size_t i = 0; i < mods.size(); i++) {
            if (mods[i].id == id) return mod_base + (int)i;
        }
        return -1;
    }

    // Whether the host player plays the song (Rush 2049's and every streamed song) rather than Rush 2's sequence player.
    bool host_song(const Song& s) {
        return s.game == Game::Rush2049 || s.id >= rush2::audio2049::stream_songs;
    }

    const std::string tab_id = "rush2_sound";
    enum class Mode : uint32_t { Original, ShuffleByGame, ShuffleAll };

    std::string song_option_id(const Song& s) {
        return std::string("song_") + s.key;
    }

    constexpr Game games[5] = { Game::Rush2, Game::Rush1, Game::Rush2049, Game::CalSpeed, Game::Mods };
    constexpr int game_count = 5;

    std::string mode_option_id(Game g) {
        return g == Game::Rush2 ? "race_music_r2" : g == Game::Rush1 ? "race_music_r1"
             : g == Game::Rush2049 ? "race_music_r49" : "race_music_cs";
    }

    // A game's songs by source (Rush 2049: N64, Dreamcast, arcade; SF Rush: N64, PlayStation, arcade), each source
    // with a switch while more than one is installed (sound.json "<game>_songs_<source>", on by default). They
    // replace Rush 2049's Soundtrack choice (soundtrack_r49, migrated at startup by src/data_files.cpp).
    struct SourceSwitch {
        Game game;
        Only only;
        const char* id;
        const char* name;
    };
    constexpr SourceSwitch source_switches[] = {
        { Game::Rush2049, Only::N64, "r49_songs_n64", "N64 Songs" },
        { Game::Rush2049, Only::Disc, "r49_songs_dc", "Dreamcast Songs" },
        { Game::Rush2049, Only::Arcade, "r49_songs_arcade", "Arcade Songs" },
        { Game::Rush1, Only::Any, "r1_songs_n64", "N64 Songs" },
        { Game::Rush1, Only::PS1, "r1_songs_ps1", "PlayStation Songs" },
        { Game::Rush1, Only::Arcade, "r1_songs_arcade", "Arcade Songs" },
    };
    constexpr int source_switch_count = (int)std::size(source_switches);
    std::array<std::atomic_bool, source_switch_count> source_on{};
    const std::string source_description =
        "Lets races play this source's songs. Switched off, a track whose own song is one of them plays its other "
        "version's song instead, and shuffles leave them out. Applies from the next song.";

    int source_switch(Game g, Only o) {
        for (int k = 0; k < source_switch_count; k++) {
            if (source_switches[k].game == g && source_switches[k].only == o) return k;
        }
        return -1;
    }

    // The Race Music choices a game offers, in the order shown.
    std::vector<Mode> game_modes(Game g) {
        if (g == Game::Rush1 || g == Game::CalSpeed) {
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
        return g == Game::Rush2 ? "games_r2" : g == Game::Rush1 ? "games_r1" : g == Game::Rush2049 ? "games_r49"
             : g == Game::CalSpeed ? "games_cs" : "games_mods";
    }

    std::string shuffle_all_option_id(Game g) {
        return g == Game::Rush2 ? "shuffle_all_r2" : g == Game::Rush1 ? "shuffle_all_r1"
             : g == Game::Rush2049 ? "shuffle_all_r49" : g == Game::CalSpeed ? "shuffle_all_cs" : "shuffle_all_mods";
    }

    const char* game_name(Game g) {
        switch (g) {
            case Game::Rush2: return "Rush 2";
            case Game::Rush1: return "SF Rush";
            case Game::Rush2049: return "Rush 2049";
            case Game::CalSpeed: return "California Speed";
            default: return "Custom Tracks";
        }
    }

    // Settings as the game threads read them.
    std::array<std::atomic<uint32_t>, game_count> modes{};      // By Game value: Race Music for the game's tracks.
    std::array<std::atomic_bool, song_count> enabled{};
    std::array<std::atomic_bool, game_count> game_enabled{};   // By Game value: the game's switch above its songs.
    std::array<std::atomic_bool, game_count> in_shuffle_all{}; // By Game value: its songs can play in a Shuffle All.
    std::mutex choice_mutex;
    std::mt19937 rng{ std::random_device{}() };
    int last_song = -1;   // Catalog index of the last race song.
    // The race's course versions' songs (catalog indices, in the order the track select's version button goes
    // through them) and the songs played this race, for SKIP TRACK (under choice_mutex).
    std::vector<int> race_variants;
    std::vector<int> race_played;
    Game race_game_now = Game::Rush2;

    // Rush 2's front-end song: sequence 9, started by command 0x0009FFFF (func_800ABE7C and func_800AD07C's state 0)
    // [V]. SF Rush's front end (screen 0, func_800B9508) plays its sequence 11 and Rush 2049's menu loop
    // (func_800D71D0) its song 6 [inferred from the callers in docs/rush1_research.md section 9 and
    // docs/rush2049_research/audio.md section 3: neither is a race song]; a Dreamcast disc plays song 6 as its
    // Select.rom. California Speed's front end (game state 0: 0x8006EFC0, 0x8006EFFC, 0x8006F368, 0x8006FA14,
    // 0x8007562C) plays its sequence 0 through 0x800569BC, the song its races also list first (Audio Track table
    // 0x800D8C64 = sequences 0-8) [V].
    constexpr uint32_t menu_command = 0x0009FFFF;
    constexpr int rush1_menu_song = 11;
    constexpr int rush2049_menu_song = 6;
    constexpr int disc_menu_song = disc + 19;   // Select.rom, the disc's menu song.
    constexpr int arcade_menu_song = arc49 + rush2::music_streams::arcade_select;   // the arcade's select.str
    constexpr int calspeed_menu_song = 0;

    // The main menu songs, each with its switch (sound.json "menu_song_<key>"). Rush 2's is on by default. Rush 2049
    // has three: the N64's song 6, the disc's Select.rom and the arcade's select screen song, each listed while its
    // source is installed.
    struct MenuSong {
        Game game;
        const char* key;
        Only only = Only::Any;   // Rush 2049's are listed under its N64 or Dreamcast heading.
    };
    constexpr std::array<MenuSong, 6> menu_songs = {{
        { Game::Rush2, "r2" },
        { Game::Rush1, "r1" },
        { Game::Rush2049, "r49", Only::N64 },
        { Game::Rush2049, "r49dc", Only::Disc },
        { Game::Rush2049, "r49arc", Only::Arcade },
        { Game::CalSpeed, "cs" },
    }};
    constexpr int menu_count = (int)menu_songs.size();
    std::array<std::atomic_bool, menu_count> menu_enabled{};
    int last_menu = -1;                     // The menu song played last (under choice_mutex).
    std::atomic<int> menu_current = -1;     // The menu song the menu plays now, -1 for Rush 2's with none on.

    std::string menu_option_id(int m) {
        return std::string("menu_song_") + menu_songs[m].key;
    }

    // Preview ids: race songs by catalog index (the mods' from mod_base), the menu songs after the built-in ones.
    constexpr int menu_preview(int m) {
        return song_count + m;
    }
    // Whether the music the game last asked for is the front end's, and a changed option waiting for the audio thread
    // to start the new menu song.
    std::atomic_bool menu_active = false;
    std::atomic_bool menu_apply = false;
    std::atomic_bool menu_reroll = false;   // The option changed: pick a new song rather than retry the last pick.
    // The race's Rush 2 or SF Rush sequence for func_8008C370's tail, or -1.
    std::atomic<int> race_sequence = -1;
    // The race's song (catalog index, -1 with MUSIC off or none known), and a song the next race music call starts
    // instead of choosing one: a skip, or the race's song again when CONTINUE restarts the music.
    std::atomic<int> race_song = -1;
    std::atomic<int> forced_song = -1;
    constexpr uint32_t music_setting = 0x800D5775;  // u8: the game's MUSIC setting (0 off, 1 per track, 2-9 a song).

    // Preview requests from the UI.
    constexpr int no_request = -2;
    constexpr int stop_request = -1;
    std::atomic<int> preview_request = no_request;  // A preview id, stop_request, or none.
    std::atomic<int> previewing = -1;   // Preview id the UI shows as playing.

    // Preview state (audio thread) and the music the game's commands last asked for (any thread that queues one);
    // both under preview_mutex.
    std::mutex preview_mutex;
    struct PreviewState {
        bool active = false;
        int song = -1;              // Preview id playing.
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

    // Whether the game's ROM (or data) is installed: its sequenced songs can play, and its column shows.
    bool rom_available(Game g) {
        switch (g) {
            case Game::Rush2: return true;
            case Game::Rush1: return rush2::track1::rom_available();
            case Game::CalSpeed: return rush2::calspeed::rom_available();
            case Game::Rush2049: return rush2::wings::rom_available();
            default: return false;
        }
    }

    // Whether Rush 2049 comes from a Dreamcast disc.
    bool disc_source() {
        auto rom = rush2::wings::get_rom();
        return rom != nullptr && rom->is_dreamcast();
    }

    // Whether a source's songs are installed at all (so its songs and switch are listed): Rush 2049's N64 ROM or
    // Dreamcast disc as the Games tab has them, a streamed source while its songs are registered.
    bool source_present(Game g, Only o) {
        switch (o) {
            case Only::N64: return rush2::wings::rom_available() && (rush2::wings::both_sources() || !disc_source());
            case Only::Disc: return rush2::wings::rom_available() && (rush2::wings::both_sources() || disc_source());
            case Only::Arcade:
                return rush2::audio2049::song_loaded(g == Game::Rush2049 ? arc49 : rock);
            case Only::PS1: return rush2::audio2049::song_loaded(ps1);
            case Only::Mod: return !mod_snapshot()->empty();
            default: return rom_available(g);
        }
    }

    // Whether a game's column shows: its ROM, or any of its sources' songs.
    bool game_available(Game g) {
        if (g == Game::Mods) return source_present(g, Only::Mod);
        if (rom_available(g)) return true;
        for (const SourceSwitch& s : source_switches) {
            if (s.game == g && source_present(g, s.only)) return true;
        }
        return false;
    }

    // How many of a game's sources are installed: with more than one, each gets its switch and heading.
    int sources_present(Game g) {
        int n = 0;
        for (const SourceSwitch& s : source_switches) {
            n += s.game == g && source_present(g, s.only);
        }
        return n;
    }

    // Whether the source's switch lets its songs play.
    bool source_allowed(Game g, Only o) {
        int k = source_switch(g, o);
        return k < 0 || source_on[(size_t)k].load(std::memory_order_relaxed);
    }

    // Whether the N64's and the disc's Rush 2049 songs may play (their switches; the other source's songs load
    // beside the first's when its switch is on, src/rush2049/track2049_audio.cpp).
    void soundtracks(bool& n64, bool& disc) {
        n64 = source_allowed(Game::Rush2049, Only::N64);
        disc = source_allowed(Game::Rush2049, Only::Disc);
    }

    // Whether a race can play catalog song i now (the 2049 sound banks load in the background).
    bool playable(int i, const std::vector<Song>& mods) {
        const Song* s = song_at(i, mods);
        if (s == nullptr || (i < song_count && !enabled[(size_t)i].load(std::memory_order_relaxed)) ||
            !game_enabled[(int)s->game].load(std::memory_order_relaxed) || !source_allowed(s->game, s->only)) {
            return false;
        }
        if (s->id >= rush2::audio2049::stream_songs) {
            return rush2::audio2049::song_loaded(s->id);
        }
        if (!rom_available(s->game) || !source_present(s->game, s->only)) {
            return false;
        }
        return s->game != Game::Rush2049 || (rush2::track2049::music_ready() && rush2::audio2049::song_loaded(s->id));
    }

    int find_song(Game g, int id) {
        for (int i = 0; i < song_count; i++) {
            if (songs[(size_t)i].game == g && songs[(size_t)i].id == id) {
                return i;
            }
        }
        return -1;
    }

    int find_key(const std::string& key) {
        for (int i = 0; i < song_count; i++) {
            if (songs[(size_t)i].key == key) return i;
        }
        return -1;
    }

    // A random playable song (of game g, or of any game in Shuffle All when all is set, or of any game at all when
    // any is set), skipping the last one when there's another. `keep`, when given, narrows the pool further.
    int pick(bool all, Game g, bool any = false, const std::function<bool(const Song&)>& keep = {}) {
        auto mods = mod_snapshot();
        std::vector<int> pool;
        for (int i : catalog(*mods)) {
            const Song& s = *song_at(i, *mods);
            bool in = any ? true : all ? in_shuffle_all[(int)s.game].load(std::memory_order_relaxed) : s.game == g;
            if (in && (!keep || keep(s)) && playable(i, *mods)) {
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

    // SF Rush's own choice on its N64 tracks: a random song of its random table, or of the PlayStation disc's (both
    // play on any SF Rush track; the arcade's belong to their tracks).
    int pick_sfrush() {
        return pick(false, Game::Rush1, false, [](const Song& s) { return s.only == Only::Any || s.only == Only::PS1; });
    }

    // A race track select id's game (what Race Music applies) and version song.
    namespace t49 = rush2::track2049;
    constexpr int r1_first = rush2::track1::first_menu_id;
    constexpr int r1_end = r1_first + rush2::track1::track_count;
    constexpr int cs_first = rush2::calspeed::first_menu_id;
    constexpr int cs_end = cs_first + rush2::calspeed::track_count;

    // A custom track's [audio] music entry as a catalog index: a song file (registered with the song player), a
    // Rush 2 song by "track1".."track8", or a Sound tab song's key. -1 if it names none.
    int mod_entry_song(const rush2::track_mod::Entry& e, const std::string& entry, const std::vector<Song>& mods) {
        if (rush2::music_streams::is_song_file(entry)) {
            std::vector<int> ids = rush2::music_streams::track_song_ids(e);
            size_t k = 0;
            for (const std::string& m : e.settings.music_list) {
                if (!rush2::music_streams::is_song_file(m)) continue;
                if (m == entry) return k < ids.size() ? find_mod_song(mods, ids[k]) : -1;
                k++;
            }
            return -1;
        }
        if (entry.size() == 6 && entry.compare(0, 5, "track") == 0 && entry[5] >= '1' && entry[5] <= '8') {
            return find_song(Game::Rush2, entry[5] - '1');
        }
        return find_key(entry);
    }

    // The arcade mods' tracks (src/arcade2049): their songs are their arcade's for their TRACKn (the 2049 arcade's
    // track<n>.str, Rush The Rock's TRACKn song).
    int arcade_track_song(const rush2::track_mod::Entry& e) {
        if (e.mod_id == rush2::arcade2049::track_mod_id) {
            for (const auto& t : rush2::arcade2049::arcade_tracks) {
                if (e.folder == t.id) return find_song(Game::Rush2049, arc49 + (t.folder[5] - '1'));
            }
        }
        if (e.mod_id == rush2::arcade2049::sfrush_track_mod_id) {
            for (const auto& t : rush2::arcade2049::sfrush_tracks) {
                if (e.folder == t.id) return find_song(Game::Rush1, rock + (t.folder[5] - '1'));
            }
        }
        return -1;
    }

    // Track select id t's game: the game whose Race Music its races follow.
    Game menu_game(int t, const std::vector<rush2::track_mod::Entry>& entries) {
        if ((t >= t49::first_menu_id && t < t49::first_menu_id + t49::track_count) || t49::is_disc_course(t)) {
            return Game::Rush2049;
        }
        if (t >= r1_first && t < r1_end) return Game::Rush1;
        if (t >= cs_first && t < cs_end) return Game::CalSpeed;
        int i = t49::mod_index(t);
        if (i >= 0 && i < (int)entries.size()) {
            const std::string& id = entries[(size_t)i].mod_id;
            if (id == rush2::arcade2049::track_mod_id) return Game::Rush2049;
            if (id == rush2::arcade2049::sfrush_track_mod_id || id == rush2::sfrush_ps1::mod_id) return Game::Rush1;
        }
        return Game::Rush2;
    }

    // Version t's own song (catalog index), -1 for none (SF Rush's and California Speed's tracks pick at random).
    int version_song(int t, const std::vector<rush2::track_mod::Entry>& entries, const std::vector<Song>& mods,
                     uint8_t* rdram) {
        if (t >= t49::first_menu_id && t < t49::first_menu_id + t49::track_count) {
            // The N64 version: the N64 ROM's song, or the disc's when the race tracks come from the disc.
            int course = t - t49::first_menu_id;
            return t49::courses_from_disc() ? find_song(Game::Rush2049, rush2::audio2049::disc_track_song(course))
                                            : find_song(Game::Rush2049, rush2::audio2049::n64_track_song(course));
        }
        if (t49::is_disc_course(t)) {
            return find_song(Game::Rush2049, rush2::audio2049::disc_track_song(t - t49::disc_menu_id));
        }
        int i = t49::mod_index(t);
        if (i >= 0 && i < (int)entries.size()) {
            const rush2::track_mod::Entry& e = entries[(size_t)i];
            if (!e.settings.music_list.empty()) return mod_entry_song(e, e.settings.music_list[0], mods);
            return arcade_track_song(e);
        }
        if (t >= 0 && t < 12) {
            return find_song(Game::Rush2, (int16_t)MEM_H(0, (int32_t)(track_songs + t * 2)));
        }
        return -1;
    }

    // What a race plays: its game, the raced version's own song and the course's versions' songs.
    struct RaceMusic {
        Game game = Game::Rush2;
        int original = -1;
        std::vector<int> variants;
    };

    RaceMusic race_music(uint8_t* rdram) {
        RaceMusic out;
        auto mods = mod_snapshot();
        int t = MEM_BU(0, (int32_t)track_id);
        bool host = t == t49::host_slot;
        // A Rush 2049 arena or the obstacle course: its 2049 track id's songs (N64's and disc's); its own is the song of
        // the source the arenas come from (Primary Source).
        int course = -1;
        if (host && t49::race_track() == t49::obstacle) {
            course = 18;
        }
        else if (t == t49::stunt_host_slot && t49::stunt_arena() > 0) {
            course = 14 + t49::stunt_arena() - 1;
        }
        else if (t == t49::stunt_host_slot && t49::battle_arena() > 0) {
            course = 6 + t49::battle_arena() - 1;
        }
        if (course >= 0) {
            out.game = Game::Rush2049;
            int n64 = find_song(Game::Rush2049, rush2::audio2049::n64_track_song(course));
            int dc = find_song(Game::Rush2049, rush2::audio2049::disc_track_song(course));
            out.original = disc_source() ? dc : n64;
            for (int k : disc_source() ? std::array<int, 2>{ dc, n64 } : std::array<int, 2>{ n64, dc }) {
                if (k >= 0) out.variants.push_back(k);
            }
            return out;
        }
        // The track select id of the version raced.
        int v = -1;
        int m = t49::raced_mod(rdram);
        if (m >= 0) {
            v = t49::mod_id(m);
        }
        else if (host && t49::race_track() >= 1 && t49::race_track() <= t49::track_count) {
            int k = t49::race_track();
            v = t49::race_track_disc() ? t49::disc_menu_id + k - 1 : t49::first_menu_id + k - 1;
        }
        else if (host && rush2::track1::race_track() > 0) {
            v = r1_first + rush2::track1::race_track() - 1;
        }
        else if (host && rush2::calspeed::race_track() > 0) {
            v = cs_first + rush2::calspeed::race_track() - 1;
        }
        else if (t >= 0 && t < 12) {
            v = t;
        }
        if (v < 0) {
            out.original = pick(false, Game::Rush2);
            return out;
        }
        std::vector<rush2::track_mod::Entry> entries = rush2::track_mod::tracks();
        std::vector<int> versions = t49::course_versions(v);
        out.game = menu_game(versions.empty() ? v : versions[0], entries);
        if (out.game == Game::Rush2) out.game = menu_game(v, entries);
        auto add_variant = [&](int song) {
            if (song >= 0 && std::find(out.variants.begin(), out.variants.end(), song) == out.variants.end()) {
                out.variants.push_back(song);
            }
        };
        for (int w : versions) {
            add_variant(version_song(w, entries, *mods, rdram));
            // A custom version's other songs ([audio] music lists several) follow its first.
            int i = t49::mod_index(w);
            if (i >= 0 && i < (int)entries.size()) {
                const auto& list = entries[(size_t)i].settings.music_list;
                for (size_t k = 1; k < list.size(); k++) add_variant(mod_entry_song(entries[(size_t)i], list[k], *mods));
            }
        }
        if (versions.empty() || std::find(versions.begin(), versions.end(), v) == versions.end()) {
            add_variant(version_song(v, entries, *mods, rdram));
        }
        out.original = version_song(v, entries, *mods, rdram);
        if (out.original < 0 && !versions.empty() && versions[0] != v) {
            // A custom version with no song of its own plays the course's.
            out.original = version_song(versions[0], entries, *mods, rdram);
        }
        if (out.original < 0) {
            if (out.game == Game::Rush1) out.original = pick_sfrush();
            else if (out.game == Game::CalSpeed) out.original = pick(false, Game::CalSpeed);
            // A custom track without a song of its own: any song the player switched on, never its host's.
            else if (m >= 0) out.original = pick(false, out.game, true);
            else out.original = pick(false, out.game);
        }
        return out;
    }

    // The course's next version song after `from` (catalog index, or -1 for the first) that can play and isn't in
    // `skip`, going round once; -1 if none.
    int next_variant(const std::vector<int>& variants, int from, const std::vector<int>& skip) {
        auto mods = mod_snapshot();
        size_t n = variants.size();
        size_t start = 0;
        auto at = std::find(variants.begin(), variants.end(), from);
        if (at != variants.end()) start = (size_t)(at - variants.begin()) + 1;
        for (size_t k = 0; k < n; k++) {
            int song = variants[(start + k) % n];
            if (song != from && std::find(skip.begin(), skip.end(), song) == skip.end() && playable(song, *mods)) {
                return song;
            }
        }
        return -1;
    }

    // A song by the game's Race Music when the race's own (or the course's versions') can't be had.
    int pick_by_mode(Game g) {
        int song = pick((Mode)modes[(int)g].load() == Mode::ShuffleAll, g);
        return song >= 0 ? song : pick(true, g);
    }

    // The race's song (catalog index), or -1 to leave it to the game. Notes the course's versions for SKIP TRACK.
    int choose(uint8_t* rdram) {
        refresh_mod_catalog();
        RaceMusic r = race_music(rdram);
        auto mods = mod_snapshot();
        int song = -1;
        switch ((Mode)modes[(int)r.game].load()) {
            case Mode::Original:
                // The raced version's own song; switched off or missing, the course's next version's.
                song = r.original >= 0 && playable(r.original, *mods) ? r.original : next_variant(r.variants, r.original, {});
                if (song < 0) song = pick(false, r.game);
                break;
            case Mode::ShuffleByGame:
                song = pick(false, r.game);
                break;
            case Mode::ShuffleAll:
                song = pick(true, r.game);
                break;
        }
        if (song < 0) {
            song = pick(true, r.game);
        }
        race_variants = r.variants;
        race_played = { song };
        std::string list;
        for (int k : r.variants) {
            if (const Song* s = song_at(k, *mods)) list += (list.empty() ? "" : ", ") + s->name + " (" + s->plays_on + ")";
        }
        fprintf(stderr, "[Music] %s course, versions' songs: %s\n", game_name(r.game), list.empty() ? "none" : list.c_str());
        race_game_now = r.game;
        return song;
    }

    // Rush 2's sequence number for a Rush 2 or SF Rush song, or -1.
    int sequence_of(uint8_t* rdram, recomp_context* ctx, const Song& s) {
        if (host_song(s)) {
            return -1;
        }
        if (s.game == Game::Rush2) {
            return (int16_t)MEM_H(0, (int32_t)(song_table + s.id * 2));
        }
        if (s.game == Game::Rush1) {
            return rush2::track1::song_sequence(rdram, ctx, s.id);
        }
        if (s.game == Game::CalSpeed) {
            return rush2::calspeed::song_sequence(rdram, ctx, s.id);
        }
        return -1;
    }

    bool rush2_song_playing(uint8_t* rdram, recomp_context* ctx) {
        recomp_context call = *ctx;
        call.r4 = MEM_W(0, (int32_t)sequence_player);
        al_seqp_get_state_800091B0(rdram, &call);
        return (int32_t)call.r2 == al_playing;
    }

    void stop_rush2_song(uint8_t* rdram, recomp_context* ctx) {
        recomp_context call = *ctx;
        music_stop_song_80061E10(rdram, &call);
    }

    // Starts sequence `sequence` on Rush 2's player; false if the loader is busy.
    bool play_rush2_song(uint8_t* rdram, recomp_context* ctx, int sequence) {
        recomp_context call = *ctx;
        call.r4 = sequence;
        call.r5 = (int32_t)-1;
        music_play_sequence_80061E68(rdram, &call);
        return (int32_t)call.r2 != 0;
    }

    // The menu song to play now: a Rush 2 or SF Rush sequence, or a Rush 2049 song (the other is -1). Games without
    // their ROM or audio fall back to Rush 2's.
    struct MenuChoice {
        int sequence = menu_command >> 16;
        int song_2049 = -1;
    };

    // Rush 2049 menu song m's song number (the N64's song 6, the disc's Select.rom or the arcade's select.str).
    int menu_song_2049(int m) {
        switch (menu_songs[m].only) {
            case Only::Disc: return disc_menu_song;
            case Only::Arcade: return arcade_menu_song;
            default: return rush2049_menu_song;
        }
    }

    // Whether menu song m can play now: its source installed, and loaded (the 2049 sound banks load in the
    // background). Its own switch is all that turns it off.
    bool menu_playable(int m) {
        const MenuSong& s = menu_songs[m];
        if (s.game != Game::Rush2049) {
            return rom_available(s.game);
        }
        int id = menu_song_2049(m);
        if (id >= rush2::audio2049::stream_songs) {
            return rush2::audio2049::song_loaded(id);
        }
        return source_present(s.game, s.only) && rush2::track2049::music_ready() && rush2::audio2049::song_loaded(id);
    }

    // Menu song m as a MenuChoice; false if it can't play (no ROM, or SF Rush's sequence not found).
    bool menu_song(uint8_t* rdram, recomp_context* ctx, int m, MenuChoice& c) {
        c = MenuChoice{};
        if (!menu_playable(m)) {
            return false;
        }
        switch (menu_songs[m].game) {
            case Game::Rush1:
                c.sequence = rush2::track1::song_sequence(rdram, ctx, rush1_menu_song);
                return c.sequence >= 0;
            case Game::CalSpeed:
                c.sequence = rush2::calspeed::song_sequence(rdram, ctx, calspeed_menu_song);
                return c.sequence >= 0;
            case Game::Rush2049:
                c.sequence = -1;
                c.song_2049 = menu_song_2049(m);
                return true;
            default:
                return true;
        }
    }

    // A random one of the menu songs switched on that can play, skipping the last one when there's another; Rush 2's
    // with none.
    MenuChoice menu_choice(uint8_t* rdram, recomp_context* ctx) {
        std::vector<int> pool;
        for (int m = 0; m < menu_count; m++) {
            if (menu_enabled[m].load() && menu_playable(m)) {
                pool.push_back(m);
            }
        }
        MenuChoice c;
        std::lock_guard lock{ choice_mutex };
        if (pool.size() > 1) {
            std::erase(pool, last_menu);
        }
        while (!pool.empty()) {
            size_t k = std::uniform_int_distribution<size_t>(0, pool.size() - 1)(rng);
            int m = pool[k];
            if (menu_song(rdram, ctx, m, c)) {
                last_menu = m;
                menu_current = m;
                return c;
            }
            pool.erase(pool.begin() + k);
        }
        menu_current = -1;
        return MenuChoice{};
    }

    // The option changed while the menu music plays: starts the new menu song (or, while a preview plays, makes it
    // what the preview returns to). False to try again (loader busy).
    bool apply_menu_music(uint8_t* rdram, recomp_context* ctx) {
        if (!menu_active) {
            return true;
        }
        // A retry (loader busy) keeps the song picked, so Shuffle doesn't re-roll every frame until one starts.
        static MenuChoice c;
        if (menu_reroll.exchange(false)) {
            c = menu_choice(rdram, ctx);
        }
        game_music = { c.sequence, c.song_2049 };
        if (preview.active) {
            return true;
        }
        if (c.song_2049 >= 0) {
            stop_rush2_song(rdram, ctx);
            rush2::track2049::play_song_now(rdram, c.song_2049);
            return true;
        }
        rush2::track2049::stop_song_now();
        return play_rush2_song(rdram, ctx, c.sequence);
    }

    // Starts preview id i as the preview. False to try again later (loader busy, 2049 banks still loading).
    bool start_preview(uint8_t* rdram, recomp_context* ctx, int i) {
        if (i >= song_count && i < mod_base) {
            int m = i - song_count;
            MenuChoice c;
            if (!menu_song(rdram, ctx, m, c)) {
                // The 2049 banks may still be loading; anything else isn't available: give up.
                return !(menu_songs[m].game == Game::Rush2049 && rom_available(Game::Rush2049) &&
                         menu_song_2049(m) < rush2::audio2049::stream_songs);
            }
            if (c.song_2049 >= 0) {
                stop_rush2_song(rdram, ctx);
                rush2::track2049::play_song_now(rdram, c.song_2049);
                preview.sequence = -1;
            }
            else {
                rush2::track2049::stop_song_now();
                if (!play_rush2_song(rdram, ctx, c.sequence)) {
                    return false;
                }
                preview.sequence = c.sequence;
            }
            preview.song = i;
            previewing = i;
            return true;
        }
        auto mods = mod_snapshot();
        const Song* song = song_at(i, *mods);
        if (song == nullptr) {
            return true; // Gone (the mods changed): give up.
        }
        if (host_song(*song)) {
            if (song->id < rush2::audio2049::stream_songs && !rush2::track2049::music_ready()) {
                return false;
            }
            stop_rush2_song(rdram, ctx);
            rush2::track2049::play_song_now(rdram, song->id);
            preview.sequence = -1;
        }
        else {
            int sequence = sequence_of(rdram, ctx, *song);
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

    // Sound tab texts, shown below the columns for the hovered or focused row.
    const std::string page_description =
        "Point at a setting to see what it does. Songs switched off aren't played in races. A game shows here once "
        "its ROM is set up (Games tab).";
    const std::string shuffle_all_description =
        "Lets this game's songs play when any game's Race Music is on Shuffle All. Switched off, they still play on "
        "this game's own tracks.";
    const std::string race_music_description =
        "How races on this game's tracks pick their song. "
        "<recomp-color primary>Original</recomp-color> plays the track's own song. "
        "<recomp-color primary>Shuffle</recomp-color> plays a random song from this game. "
        "<recomp-color primary>Shuffle All</recomp-color> plays a random song from any game in Shuffle All. "
        "Only used while the game's own MUSIC setting is on its default.";

    // A row of the page: shows its description when hovered or focused, and scrolls itself into view when something
    // in it takes focus, so the columns follow the d-pad.
    Element* add_row(Element* parent, std::function<void()> on_show) {
        Element* row = get_current_context().create_element<rush2::ui::FocusRow>(parent, std::move(on_show));
        row->set_display(Display::Flex);
        row->set_flex_direction(FlexDirection::Row);
        row->set_align_items(AlignItems::Center);
        row->set_gap(12.0f);
        row->set_min_height(48.0f);
        // A grid row of its column (a grid column): up and down keep the place in the row, so the d-pad goes from a
        // song's switch to the next song's switch rather than to its play button.
        row->set_as_navigation_container(NavigationType::GridRow);
        return row;
    }

    // The play/stop button: one element whose icon changes, so it keeps focus when pressed.
    class PreviewButton : public IconButton {
    public:
        using IconButton::IconButton;

        void show_playing(bool playing) {
            svg->set_src(playing ? "icons/Stop.svg" : "icons/Play.svg");
            apply_button_style(playing ? ButtonStyle::Primary : ButtonStyle::Secondary);
        }
    };

    // Laid out like the Cheats tab: the general settings and each installed game's songs in columns, with the hovered
    // or focused row's description below them. Columns have fixed widths (add_column); more than fit scroll sideways,
    // by the arrows beside them (a column at a time) or by the d-pad (each row scrolls itself into view).
    class SoundPage : public ConfigPage {
    public:
        SoundPage(ResourceId rid, Element* parent) : ConfigPage(rid, parent, Events(EventType::Update)) {
            ContextId context = get_current_context();
            set_as_navigation_container(NavigationType::Vertical);

            // Placed rather than flexed down to the row of columns: RmlUi lays out nested flex boxes many times over,
            // and with them this page took tens of milliseconds to lay out on every description change
            // (tools/rml_layout_bench).
            constexpr float description_height = 92.0f;
            body->get_right()->set_display(Display::None);
            Element* left = body->get_left();
            left->set_display(Display::Block);
            left->set_position(Position::Relative);
            left->set_height(100.0f, Unit::Percent);

            // The columns with an arrow on each side.
            Element* strip = context.create_element<Element>(left, 0, "div", false);
            strip->set_position(Position::Absolute);
            strip->set_top(16.0f);
            strip->set_left(16.0f);
            strip->set_right(16.0f);
            strip->set_bottom(description_height + 24.0f);

            arrows[0] = add_arrow(strip, "icons/CaretLeft.svg", -1);
            grid = context.create_element<Element>(strip, 0, "div", false);
            grid->set_position(Position::Absolute);
            grid->set_top(0.0f);
            grid->set_bottom(0.0f);
            grid->set_left(arrow_room);
            grid->set_right(arrow_room);
            grid->set_display(Display::Flex);
            grid->set_flex_direction(FlexDirection::Row);
            grid->set_align_items(AlignItems::FlexStart);
            grid->set_gap(column_gap);
            grid->set_overflow_x(Overflow::Auto);    // Auto, not Hidden: rows can only scroll themselves into view in a scrolling box.
            grid->set_overflow_y(Overflow::Auto);
            grid->set_as_navigation_container(NavigationType::Horizontal);
            arrows[1] = add_arrow(strip, "icons/CaretRight.svg", 1);

            description = context.create_element<Element>(left, 0, "p", true);
            description->set_position(Position::Absolute);
            description->set_left(16.0f);
            description->set_right(16.0f);
            description->set_bottom(16.0f);
            description->set_typography(theme::Typography::Body);
            description->set_line_height(28.0f);
            description->set_padding_top(8.0f);
            description->set_height(description_height);
            description->set_color(theme::color::TextDim);
            description->set_border_top_width(1.0f);
            description->set_border_top_color(theme::color::Border);
            description->set_text_unsafe(page_description);

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
                update_arrows();
                queue_update();
            }
        }

    private:
        static constexpr float column_gap = 24.0f;
        static constexpr float arrow_room = 48.0f;   // Each side of the columns, for its arrow (a small icon button).

        struct Row {
            Element* row;
            PreviewButton* button;
            Toggle* toggle;     // nullptr for a mod's song (the Mods column's switch covers them)
            Label* name;
            int id;     // Preview id: a race song's catalog index, or menu_preview(m).
            Game game;
            Only only;
        };
        struct Header {
            Element* column = nullptr;
            Toggle* toggle = nullptr;
            Select* modes = nullptr;
            Toggle* shuffle_all = nullptr;
            // The game's source switches (source_switches order) and its sources' headings over their songs, shown
            // while more than one of its sources is installed.
            std::array<Element*, source_switch_count> source_rows{};
            std::map<Only, Element*> headings;
            bool shown = false;
        };
        Element* grid = nullptr;
        Element* description = nullptr;
        IconButton* arrows[2] = {};     // Left, right.
        int shown_arrows[2] = { 0, 0 };
        Element* general = nullptr;
        std::vector<Row> rows;
        Header headers[game_count];
        int shown_preview = -1;
        int shown_state[game_count] = { -1, -1, -1, -1, -1 };

        static int game_state(Game g) {
            int state = (game_available(g) ? 1 : 0) | (game_enabled[(int)g].load() ? 2 : 0);
            int bit = 4;
            for (int k = 0; k < source_switch_count; k++) {
                if (source_switches[k].game != g) continue;
                state |= (source_present(g, source_switches[k].only) ? bit : 0) |
                         (source_on[(size_t)k].load() ? bit << 1 : 0);
                bit <<= 2;
            }
            return state;
        }

        void show(const std::string& text) {
            description->set_text_unsafe(text);
        }

        void show_preview(int now) {
            for (Row& r : rows) {
                if (r.id == now || r.id == shown_preview) {
                    r.button->show_playing(r.id == now);
                }
            }
            shown_preview = now;
        }

        // A game's column shows while its ROM (or one of its sources) is there. A source's songs are listed while it is
        // installed; its race songs' switches also need the game's switch and the source's on.
        void refresh_game(Game g) {
            int state = game_state(g);
            bool available = (state & 1) != 0;
            bool on = (state & 2) != 0;
            bool several = sources_present(g) > 1;
            Header& h = headers[(int)g];
            h.column->set_display(available ? Display::Block : Display::None);
            h.shown = available;
            for (int k = 0; k < source_switch_count; k++) {
                if (Element* row = h.source_rows[(size_t)k]) {
                    row->set_display(several && source_present(g, source_switches[k].only) ? Display::Flex : Display::None);
                }
            }
            for (auto& [only, heading] : h.headings) {
                heading->set_display(several && source_present(g, only) ? Display::Block : Display::None);
            }
            for (Row& r : rows) {
                if (r.game != g) {
                    continue;
                }
                bool menu = r.id >= song_count && r.id < mod_base;
                bool listed = source_present(g, r.only);
                bool active = available && (menu || (on && source_allowed(g, r.only)));
                r.row->set_display(listed ? Display::Flex : Display::None);
                r.button->set_enabled(available);
                if (r.toggle != nullptr) r.toggle->set_enabled(active);
                r.name->set_color(active ? theme::color::Text : theme::color::TextInactive);
            }
            h.toggle->set_enabled(available);
            if (h.modes != nullptr) h.modes->set_enabled(available);
            h.shuffle_all->set_enabled(available);
            shown_state[(int)g] = state;
        }

        // An arrow beside the columns: scrolls them a column toward `direction` (-1 left, 1 right). Mouse only: the
        // d-pad scrolls the columns by moving through them.
        IconButton* add_arrow(Element* strip, const char* icon, int direction) {
            ContextId context = get_current_context();
            // Vertically centered at its side of the strip.
            Element* holder = context.create_element<Element>(strip, 0, "div", false);
            holder->set_position(Position::Absolute);
            holder->set_top(50.0f, Unit::Percent);
            holder->set_translate_2D(0.0f, -50.0f, Unit::Percent);
            if (direction < 0) {
                holder->set_left(0.0f);
            }
            else {
                holder->set_right(0.0f);
            }
            IconButton* arrow = context.create_element<IconButton>(holder, icon, ButtonStyle::Secondary, IconButtonSize::Small);
            // Out of d-pad navigation by its tab index, not set_focusable(false): RmlUi clicks the nearest focusable
            // element under the mouse, so an unfocusable arrow's clicks went to the page instead.
            arrow->set_tab_index(TabIndex::None);
            arrow->set_visibility(Visibility::Hidden);    // Until update_arrows finds a column cut off on its side.
            arrow->add_pressed_callback([this, direction]() { scroll_columns(direction); });
            return arrow;
        }

        // The columns shown, with their left and right edges on screen.
        struct Span {
            float left, right;
        };
        std::vector<Span> shown_columns() {
            std::vector<Span> spans;
            auto add = [&spans](Element* c) {
                float left = c->get_absolute_left();
                spans.push_back({ left, left + c->get_client_width() });
            };
            add(general);
            for (Game g : games) {
                if (headers[(int)g].shown) {
                    add(headers[(int)g].column);
                }
            }
            return spans;
        }

        // Scrolls so the next column cut off toward `direction` lines up with that edge of the grid.
        void scroll_columns(int direction) {
            float view_left = grid->get_absolute_left();
            float view_right = view_left + grid->get_client_width();
            float scroll = grid->get_scroll_left();
            std::vector<Span> spans = shown_columns();
            if (direction > 0) {
                for (const Span& s : spans) {
                    if (s.right > view_right + 1.0f) {
                        grid->set_scroll_left(scroll + s.right - view_right, true);
                        return;
                    }
                }
            }
            else {
                for (auto s = spans.rbegin(); s != spans.rend(); ++s) {
                    if (s->left < view_left - 1.0f) {
                        grid->set_scroll_left(std::max(0.0f, scroll - (view_left - s->left)), true);
                        return;
                    }
                }
            }
        }

        // Each arrow shows while there's a column cut off on its side. Both always keep their room, so the columns
        // never move when one shows.
        void update_arrows() {
            float view_left = grid->get_absolute_left();
            float view_right = view_left + grid->get_client_width();
            bool more[2] = { false, false };
            for (const Span& s : shown_columns()) {
                more[0] |= s.left < view_left - 1.0f;
                more[1] |= s.right > view_right + 1.0f;
            }
            for (int k = 0; k < 2; k++) {
                if ((int)more[k] != shown_arrows[k]) {
                    shown_arrows[k] = more[k];
                    arrows[k]->set_visibility(more[k] ? Visibility::Visible : Visibility::Hidden);
                }
            }
        }

        // Column widths (dp): each fits its widest row with at least 16dp between a name and its control (General:
        // "Music While Paused" and its switch; a game: "Race Music" and its select; California Speed: its title and
        // switch). General through Rush 2049 fit a 16:9 page beside the arrows; columns share what's left over.
        static constexpr float general_width = 396.0f;
        static constexpr float game_width = 390.0f;
        static constexpr float calspeed_width = 430.0f;

        Element* add_column(float width) {
            ContextId context = get_current_context();
            // A block, not a flex column: only the row of columns is flexed.
            Element* column = context.create_element<Element>(grid, 0, "div", false);
            column->set_display(Display::Block);
            column->set_flex_grow(1.0f);
            column->set_flex_shrink(0.0f);
            column->set_flex_basis(width, Unit::Dp);
            column->set_as_navigation_container(NavigationType::GridCol);
            return column;
        }

        // A row's name: it takes up the row's slack, so the control ends the row.
        Label* add_name(Element* row, const std::string& text, theme::Typography typography) {
            Label* label = get_current_context().create_element<Label>(row, text, typography);
            label->set_flex_grow(1.0f);
            label->set_white_space(WhiteSpace::Nowrap);
            return label;
        }

        // A column's title row, underlined like the Cheats tab's group titles.
        Element* add_title(Element* column, const std::string& title, std::function<void()> on_show) {
            Element* row = add_row(column, std::move(on_show));
            row->set_padding_bottom(6.0f);
            row->set_margin_bottom(4.0f);
            row->set_border_bottom_width(1.0f);
            row->set_border_bottom_color(theme::color::Border);
            add_name(row, title, theme::Typography::LabelLG);
            return row;
        }

        // A setting's row: its name, then the control added to it.
        Element* add_setting(Element* column, const std::string& name, const std::string& text) {
            Element* row = add_row(column, [this, text]() { show(text); });
            add_name(row, name, theme::Typography::LabelMD);
            return row;
        }

        // A switch in a row, kept in the sound config under `id`.
        Toggle* add_switch(Element* row, const std::string& id) {
            Toggle* toggle = get_current_context().create_element<Toggle>(row, ToggleSize::Medium);
            toggle->set_flex_shrink(0.0f);
            toggle->set_checked(std::get<bool>(recompui::config::get_sound_config().get_option_value(id)));
            toggle->add_checked_callback([id](bool checked) {
                recompui::config::get_sound_config().set_option_value(id, checked);
            });
            return toggle;
        }

        // A select in a row: it fills its parent's width, so it gets a parent of its own.
        Select* add_select(Element* row, std::vector<SelectOption> options, const std::string& selected) {
            ContextId context = get_current_context();
            Element* holder = context.create_element<Element>(row, 0, "div", false);
            holder->set_width(192.0f);    // The select's minimum width.
            holder->set_flex_shrink(0.0f);
            Select* select = context.create_element<Select>(holder, std::move(options), selected);
            select->set_tab_index_auto();
            return select;
        }

        // A song's row: its play button, its name and its switch (option `option_id`; none when empty).
        void add_song_row(Element* column, int id, Game game, Only only, const std::string& name,
                          const std::string& option_id, std::function<void()> on_show) {
            ContextId context = get_current_context();
            Element* row = add_row(column, std::move(on_show));
            PreviewButton* button = context.create_element<PreviewButton>(row, "icons/Play.svg", ButtonStyle::Secondary, IconButtonSize::Medium);
            button->set_flex_shrink(0.0f);
            button->add_pressed_callback([id]() {
                preview_request = previewing.load() == id ? stop_request : id;
            });
            Label* label = add_name(row, name, theme::Typography::LabelMD);
            Toggle* toggle = option_id.empty() ? nullptr : add_switch(row, option_id);
            rows.push_back({ row, button, toggle, label, id, game, only });
        }

        static const char* source_heading(Only only) {
            switch (only) {
                case Only::Disc: return "Dreamcast";
                case Only::Arcade: return "Arcade";
                case Only::PS1: return "PlayStation";
                default: return "N64";
            }
        }

        void build() {
            ContextId context = get_current_context();
            recomp::config::Config& config = recompui::config::get_sound_config();
            refresh_mod_catalog();
            auto mods = mod_snapshot();

            general = add_column(general_width);
            add_title(general, "General", [this]() { show(page_description); });

            const auto& volume_config = config.get_option(recompui::config::sound::options::main_volume);
            Element* volume_row = add_setting(general, "Main Volume", volume_config.description);
            // The slider gets a line of its own under the name, so it keeps a usable length.
            volume_row->set_flex_direction(FlexDirection::Column);
            volume_row->set_align_items(AlignItems::Stretch);
            volume_row->set_gap(4.0f);
            volume_row->set_padding_top(8.0f);
            volume_row->set_padding_bottom(8.0f);
            Element* volume_holder = context.create_element<Element>(volume_row, 0, "div", false);
            Slider* volume = context.create_element<Slider>(volume_holder, SliderType::Percent);
            const auto& volume_option = std::get<recomp::config::ConfigOptionNumber>(volume_config.variant);
            volume->set_min_value(volume_option.min);
            volume->set_max_value(volume_option.max);
            volume->set_step_value(volume_option.step);
            volume->set_precision(volume_option.precision);
            volume->set_value(recompui::config::sound::get_main_volume());
            volume->add_value_changed_callback([](double value) {
                recompui::config::get_sound_config().set_option_value(recompui::config::sound::options::main_volume, value);
            });

            add_switch(add_setting(general, "Other Cars' Engines", other_engines_description), other_engines_id);
            add_switch(add_setting(general, "Music While Paused", pause_music_description), pause_music_id);
            Select* announcer = add_select(add_setting(general, "Announcer", announcer_description), {
                    { "Rush 2", std::to_string((uint32_t)rush2::announcer::Choice::Rush2) },
                    { "Cal. Speed", std::to_string((uint32_t)rush2::announcer::Choice::CalSpeed) },
                }, std::to_string(std::get<uint32_t>(config.get_option_value(announcer_id))));
            announcer->add_change_callback([](SelectOption& choice, int) {
                recompui::config::get_sound_config().set_option_value(announcer_id, (uint32_t)std::stoul(choice.value));
            });

            for (Game g : games) {
                // The game's column: its title with the game's switch, its Race Music, source switches, In Shuffle All
                // and menu song, then its race songs. The custom tracks' column has no Race Music (their races follow
                // Rush 2's) and no switch per song.
                Element* column = add_column(g == Game::CalSpeed ? calspeed_width : game_width);
                Header& h = headers[(int)g];
                h.column = column;
                Element* title = add_title(column, game_name(g), [this, g]() {
                    show(g == Game::Mods
                        ? std::string("Lets races play the custom tracks' own songs (their track.toml [audio] music). "
                                      "Each custom track still plays its own song as its Original.")
                        : std::string("Lets races play ") + game_name(g) + "'s songs. Switched off, each song keeps "
                          "its own switch for when it's back on.");
                });
                h.toggle = add_switch(title, game_option_id(g));

                if (g != Game::Mods) {
                    Element* mode_row = add_setting(column, "Race Music", race_music_description);
                    std::vector<SelectOption> mode_options;
                    for (Mode m : game_modes(g)) {
                        mode_options.emplace_back(mode_name(m), std::to_string((uint32_t)m));
                    }
                    h.modes = add_select(mode_row, mode_options,
                        std::to_string(std::get<uint32_t>(config.get_option_value(mode_option_id(g)))));
                    h.modes->add_change_callback([g](SelectOption& choice, int) {
                        recompui::config::get_sound_config().set_option_value(mode_option_id(g),
                            (uint32_t)std::stoul(choice.value));
                    });
                }

                for (int k = 0; k < source_switch_count; k++) {
                    if (source_switches[k].game != g) continue;
                    Element* row = add_setting(column, source_switches[k].name, source_description);
                    add_switch(row, source_switches[k].id);
                    h.source_rows[(size_t)k] = row;
                }

                h.shuffle_all = add_switch(add_setting(column, "In Shuffle All", shuffle_all_description),
                                           shuffle_all_option_id(g));

                // The game's menu song; a game with several sources lists each source's under its heading.
                auto add_menu_rows = [&](Only only) {
                    for (int m = 0; m < menu_count; m++) {
                        if (menu_songs[m].game == g && menu_songs[m].only == only) {
                            add_song_row(column, menu_preview(m), g, only, "Main Menu", menu_option_id(m),
                                [this]() { show(menu_song_description); });
                        }
                    }
                };
                add_menu_rows(Only::Any);

                // The sources share song names (Rush 2049's N64 and disc songs, SF Rush's "Song 1"s): each source's
                // songs go under its heading, shown while the game has more than one source installed.
                bool first = true;
                Only last = Only::Any;
                for (int i : catalog(*mods)) {
                    const Song& song = *song_at(i, *mods);
                    if (song.game != g) {
                        continue;
                    }
                    if (g != Game::Mods && (first || song.only != last)) {
                        Element* heading = context.create_element<Label>(column, source_heading(song.only),
                                                                         theme::Typography::LabelSM);
                        heading->set_color(theme::color::TextDim);
                        heading->set_padding_top(8.0f);
                        h.headings[song.only] = heading;
                        if (song.only != Only::Any) add_menu_rows(song.only);
                    }
                    first = false;
                    last = song.only;
                    std::string text = std::string("<recomp-color primary>") + song.name + "</recomp-color>. Plays on: " +
                                       song.plays_on + (i >= mod_base ? ". Press play to hear it."
                                                                      : ". Press play to hear it, or switch it off to "
                                                                        "keep it out of races.");
                    add_song_row(column, i, song.game, song.only, song.name, i >= mod_base ? "" : song_option_id(song),
                                 [this, text]() { show(text); });
                }
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
    config.add_bool_option(pause_music_id, "Keep Music Playing When Paused", pause_music_description, false, true);
    config.add_enum_option(announcer_id, "Announcer", announcer_description,
        std::vector<recomp::config::ConfigOptionEnumOption>{
            { rush2::announcer::Choice::Rush2, "Rush2", "Rush 2" },
            { rush2::announcer::Choice::CalSpeed, "CalSpeed", "Cal. Speed" },
        },
        rush2::announcer::Choice::Rush2, true);
    config.add_option_change_callback(announcer_id,
        [](recomp::config::ConfigValueVariant cur_value, recomp::config::ConfigValueVariant, recomp::config::OptionChangeContext) {
            rush2::announcer::set((rush2::announcer::Choice)std::get<uint32_t>(cur_value));
        });
    config.add_option_change_callback(pause_music_id,
        [](recomp::config::ConfigValueVariant cur_value, recomp::config::ConfigValueVariant, recomp::config::OptionChangeContext) {
            pause_keeps_music = std::get<bool>(cur_value);
        });
    for (int m = 0; m < menu_count; m++) {
        Game g = menu_songs[m].game;
        menu_enabled[m] = g == Game::Rush2;
        config.add_bool_option(menu_option_id(m), std::string(game_name(g)) +
            (menu_songs[m].only == Only::Disc ? " Dreamcast Menu Song"
             : menu_songs[m].only == Only::Arcade ? " Arcade Menu Song" : " Menu Song"), menu_song_description,
            g == Game::Rush2, true);
        config.add_option_change_callback(menu_option_id(m),
            [m](recomp::config::ConfigValueVariant cur_value, recomp::config::ConfigValueVariant, recomp::config::OptionChangeContext) {
                menu_enabled[m] = std::get<bool>(cur_value);
                // The menu's song changes at once only when it was switched off, or when it was Rush 2's for want of
                // any switched on.
                int current = menu_current.load();
                if (current < 0 || !menu_enabled[current].load()) {
                    menu_reroll = true;
                    menu_apply = true;
                }
            });
    }
    for (Game g : games) {
        in_shuffle_all[(int)g] = true;
        config.add_bool_option(shuffle_all_option_id(g), std::string(game_name(g)) + " In Shuffle All",
            shuffle_all_description, true, true);
        config.add_option_change_callback(shuffle_all_option_id(g),
            [g](recomp::config::ConfigValueVariant cur_value, recomp::config::ConfigValueVariant, recomp::config::OptionChangeContext) {
                in_shuffle_all[(int)g] = std::get<bool>(cur_value);
            });
    }
    for (Game g : games) {
        if (g == Game::Mods) {
            continue; // Custom tracks' races follow Rush 2's Race Music.
        }
        std::vector<recomp::config::ConfigOptionEnumOption> choices;
        for (Mode m : game_modes(g)) {
            const char* key = m == Mode::Original ? "Original" : m == Mode::ShuffleByGame ? "ShuffleByGame" : "ShuffleAll";
            choices.push_back({ m, key, mode_name(m) });
        }
        modes[(int)g] = (uint32_t)Mode::Original;
        config.add_enum_option(mode_option_id(g), std::string(game_name(g)) + " Race Music", race_music_description, choices,
            Mode::Original, true);
        config.add_option_change_callback(mode_option_id(g),
            [g](recomp::config::ConfigValueVariant cur_value, recomp::config::ConfigValueVariant, recomp::config::OptionChangeContext) {
                modes[(int)g] = std::get<uint32_t>(cur_value);
            });
    }
    for (int k = 0; k < source_switch_count; k++) {
        const SourceSwitch& sw = source_switches[k];
        source_on[(size_t)k] = true;
        config.add_bool_option(sw.id, std::string(game_name(sw.game)) + " " + sw.name, source_description, true, true);
        config.add_option_change_callback(sw.id,
            [k](recomp::config::ConfigValueVariant cur_value, recomp::config::ConfigValueVariant, recomp::config::OptionChangeContext) {
                source_on[(size_t)k] = std::get<bool>(cur_value);
                // Starts loading the other Rush 2049 source's songs (on a worker thread) if they're now wanted.
                rush2::track2049::music_ready();
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
        const Song& s = songs[(size_t)i];
        enabled[(size_t)i] = true;
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
    pause_keeps_music = std::get<bool>(config.get_option_value(pause_music_id));
    rush2::announcer::set((rush2::announcer::Choice)std::get<uint32_t>(config.get_option_value(announcer_id)));
    for (int m = 0; m < menu_count; m++) {
        menu_enabled[m] = std::get<bool>(config.get_option_value(menu_option_id(m)));
    }
    for (Game g : games) {
        if (g != Game::Mods) {
            modes[(int)g] = std::get<uint32_t>(config.get_option_value(mode_option_id(g)));
        }
        in_shuffle_all[(int)g] = std::get<bool>(config.get_option_value(shuffle_all_option_id(g)));
    }
    for (int i = 0; i < song_count; i++) {
        enabled[i] = std::get<bool>(config.get_option_value(song_option_id(songs[i])));
    }
    for (Game g : games) {
        game_enabled[(int)g] = std::get<bool>(config.get_option_value(game_option_id(g)));
    }
    for (int k = 0; k < source_switch_count; k++) {
        source_on[(size_t)k] = std::get<bool>(config.get_option_value(source_switches[k].id));
    }
    // Start loading Rush 2049's sound banks now, so its songs are ready for the first race.
    if (rush2::wings::rom_available()) {
        rush2::track2049::music_ready();
    }
    // Find the streamed songs (the arcade's, the PlayStation disc's, the custom tracks') off the main thread.
    std::thread([]() {
        rush2::music_streams::refresh_sources();
        refresh_mod_catalog();
    }).detach();
}

// func_8008C370 entry: $a0 = 2 starts the race's music with setting $a1.
extern "C" void rush2_music_race(uint8_t* rdram, recomp_context* ctx) {
    if (pause_hold.load()) {
        return; // Continuing from the pause with the song still playing.
    }
    pause_music = -1;
    race_sequence = -1;
    rush2::track2049::queue_race_song(-1);
    if ((int32_t)ctx->r4 != 2) {
        return;
    }
    int song = forced_song.exchange(-1);
    uint32_t setting = ctx->r5 & 0xFF;
    std::lock_guard lock{ choice_mutex };
    if (song >= 0 && setting != 0) {
        ctx->r5 = 1; // Rush 2's per track path, whose sequence is replaced below.
    }
    else if (setting != 1) {
        // Off or one of Rush 2's songs, left to the game.
        race_song = setting >= 2 && setting <= 9 ? find_song(Game::Rush2, (int)setting - 2) : -1;
        return;
    }
    else {
        song = choose(rdram);
    }
    race_song = song;
    if (song < 0) {
        return;
    }
    auto mods = mod_snapshot();
    const Song* found = song_at(song, *mods);
    if (found == nullptr) {
        race_song = -1;
        return;
    }
    const Song& s = *found;
    if (host_song(s)) {
        rush2::track2049::queue_race_song(s.id);
        ctx->r5 = 0;
    }
    else {
        int sequence = sequence_of(rdram, ctx, s);
        if (sequence < 0) {
            race_song = -1;
            return;
        }
        race_sequence = sequence;
    }
    last_song = song;
    fprintf(stderr, "[Music] Race song: %s %s (%s)\n", game_name(s.game), s.name.c_str(), s.plays_on.c_str());
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
    // The front end's song, swapped for a menu song switched on (menu_choice): SF Rush's as a sequence on Rush 2's player, Rush 2049's
    // as a queued song started by Rush 2's stop command.
    uint32_t kind = cmd >> 30;
    if (kind == 3) {
        return false; // Volume and fade.
    }
    if (pause_hold.load()) {
        ctx->r4 = (int32_t)command_volume;
        return true;
    }
    menu_active = cmd == menu_command;
    if (menu_active) {
        MenuChoice c = menu_choice(rdram, ctx);
        if (c.song_2049 >= 0) {
            rush2::track2049::queue_race_song(c.song_2049);
            cmd = 0x40000000;
        }
        else {
            cmd = ((uint32_t)c.sequence << 16) | 0xFFFF;
        }
        ctx->r4 = (int32_t)cmd;
        kind = cmd >> 30;
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
    rush2::sfx_sweep::audio_frame(rdram, ctx); // test aid, off unless RUSH2_TEST_SFX_SWEEP is set
    if (menu_apply.load()) {
        std::lock_guard lock{ preview_mutex };
        if (menu_apply.exchange(false) && !apply_menu_music(rdram, ctx)) {
            menu_apply = true;
        }
    }
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
    if (request < 0 || (request >= menu_preview(menu_count) && request < mod_base)) {
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

// race_pause_menu_step (func_800AFD44) before the pause's music stop (0x800AFE38): held while Keep Music Playing When
// Paused is on. Before CONTINUE restarts the song (0x800B00B8): held while the song still plays.
extern "C" void rush2_music_pause_start(uint8_t* rdram, recomp_context* ctx) {
    bool keep = pause_keeps_music.load();
    pause_hold = keep;
    pause_music = keep ? 1 : 0;
}

extern "C" void rush2_music_continue(uint8_t* rdram, recomp_context* ctx) {
    pause_hold = pause_music.load() == 1;
    pause_music = -1;
    // A stopped song starts again from the top, the same song (a shuffle would otherwise pick another).
    forced_song = pause_hold.load() ? -1 : race_song.load();
}

// After those calls (0x800AFE48, 0x800B00C0).
extern "C" void rush2_music_pause_release(uint8_t* rdram, recomp_context* ctx) {
    pause_hold = false;
}

// race_pause_menu_step (func_800AFD44) entry, every race frame: while the pause menu is open (0x8002305C), applies a
// change of Keep Music Playing When Paused at once.
extern "C" void rush2_music_pause_frame(uint8_t* rdram, recomp_context* ctx) {
    constexpr uint32_t pause_state = 0x8002305C;   // Nonzero while the pause menu (or one of its screens) is open.
    constexpr uint32_t race_state = 0x8010C0D0;    // u32; 0xA: a race without music restarts (func_800AFD44)
    int playing = pause_music.load();
    if (playing < 0 || MEM_B(0, (int32_t)pause_state) == 0) {
        return;
    }
    bool want = pause_keeps_music.load();
    if (want == (playing == 1)) {
        return;
    }
    recomp_context call = *ctx;
    if (want) {
        if (MEM_W(0, (int32_t)race_state) != 0xA) {
            forced_song = race_song.load();
            call.r4 = 2;
            call.r5 = MEM_BU(0, (int32_t)music_setting);
            music_pick_race_song_8008C370(rdram, &call);
        }
        pause_music = 1;
    }
    else {
        call.r4 = 0x40000000;
        music_queue_command_80062F50(rdram, &call);
        pause_music = 0;
    }
}

// The pause menu's SKIP TRACK (src/pause_menu.cpp), on the game thread: first the course's other versions' songs (the
// next one after the song playing, in the track select's version order, that hasn't played this race), then another
// song by the game's Race Music (Shuffle All's pool with Shuffle All, else the game's; any game's when the game has no
// other), never the one playing. A song playing in the pause (Keep Music Playing When Paused) changes at once through
// func_8008C370, as a race's start does; a stopped one becomes the song CONTINUE starts.
bool rush2::music::skip_song(uint8_t* rdram, recomp_context* ctx) {
    uint32_t setting = MEM_BU(0, (int32_t)music_setting);
    if (setting == 0) {
        return false; // MUSIC is off.
    }
    int song;
    {
        std::lock_guard lock{ choice_mutex };
        Game g = race_game_now;
        int current = race_song.load();
        if (current >= 0) {
            last_song = current;
        }
        song = next_variant(race_variants, current, race_played);
        if (song < 0) {
            song = pick_by_mode(g);
        }
        if (song < 0 || song == current) {
            return false;
        }
        race_played.push_back(song);
        last_song = song;
        auto mods = mod_snapshot();
        if (const Song* s = song_at(song, *mods)) {
            fprintf(stderr, "[Music] Skip to %s %s (%s)\n", game_name(s->game), s->name.c_str(), s->plays_on.c_str());
        }
    }
    int playing = pause_music.load();
    if (playing == 0) {
        race_song = song;
        return true;
    }
    forced_song = song;
    recomp_context call = *ctx;
    call.r4 = 2;
    call.r5 = (int32_t)setting;
    music_pick_race_song_8008C370(rdram, &call);
    pause_music = playing;
    return true;
}

void rush2::music::rush2049_soundtracks(bool& n64, bool& disc) {
    soundtracks(n64, disc);
}

bool rush2::music::now_playing(NowPlaying& out) {
    auto mods = mod_snapshot();
    const Song* found = song_at(race_song.load(), *mods);
    if (found == nullptr) {
        return false;
    }
    const Song& s = *found;
    out.song = s.name;
    out.game = game_name(s.game);
    // Where the song plays: Rush 2's track, Rush 2049's courses (without the game's name; the arcade's say "Arcade"),
    // a custom track's name; SF Rush and California Speed play their songs on any of their tracks.
    std::string where = (s.game == Game::Rush1 && s.only != Only::Arcade) || s.game == Game::CalSpeed ? "" : s.plays_on;
    if (where.rfind("Rush 2049 ", 0) == 0) {
        where = where.substr(10);
    }
    else if (where.rfind("SF Rush ", 0) == 0) {
        where = where.substr(8);
    }
    else if (where.rfind("No Rush 2049", 0) == 0 || where.rfind("No track", 0) == 0) {
        where = "";
    }
    out.course = where;
    return true;
}
