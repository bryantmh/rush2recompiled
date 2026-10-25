// Race music across Rush 2, SF Rush, Rush 2049 and California Speed: the Sound tab's song list, how a race picks its
// song, and song previews.
//
// Songs: the ones the games play on their race tracks. Rush 2's eight (its MUSIC setting's 2-9, sequences
// 0x800CC394 = [3, 4, 0, 1, 7, 10, 2, 8]; per-track choice 0x800CC37C), SF Rush's nine race songs (its random table
// 0x800D2910, played through Rush 2's player as sequences 13-28, src/rush1/track1_audio.cpp) and the six songs of Rush
// 2049's race tracks (0x8010FFD4, played on the host, src/rush2049/track2049_audio.cpp). A Rush 2049 Dreamcast disc has its own
// 20 songs (rush2::audio2049::disc_songs, a song per track at 0x8C0BA76C), listed instead of the N64's while it's the
// 2049 source. With both the N64 ROM and the disc installed, the Rush 2049 Soundtrack option picks the N64's, the
// disc's or both (the other source's songs then load beside the first's, audio2049::load_music); with both, a 2049
// track's Original is its N64 or its disc song and the menu plays either game's menu song. California Speed's nine songs (its song file, ROM 0xD69FC0) play through Rush 2's player as sequences
// 29-37 (src/rush1/track1_audio.cpp). California Speed has no per-track songs (the player picks one: its options'
// Audio Track), so like SF Rush its tracks play a random one of its songs by default.
//
// Choice: Rush 2 picks a race's song in func_8008C370(2, setting), setting 1 being "per track" (byte 0x800D5775).
// Only that setting is taken over; the fixed-song settings and Off stay the game's. Each game's Race Music option
// chooses for that game's tracks: Original (the game's own choice, swapped for another song of that game, then of any
// game, when it is switched off), Shuffle (a random song of the game) or Shuffle All (a random song of any
// game in Shuffle All). SF Rush and California Speed offer only Original and Shuffle All: their own choice already is
// a random song of the game. A game's In Shuffle All switch keeps its songs out of every Shuffle All (its own tracks'
// Original and Shuffle still play them). A custom track counts as a Rush 2 track; one with no song of its own
// ([audio] music) has as its Original a random song of any game, of every song switched on.
// Shuffles skip the song played last. A song of a game whose ROM or audio isn't available is skipped; with nothing
// left the game picks as it would by itself. A Rush 2 or SF Rush song replaces the sequence Rush 2 chose (at
// 0x8008C46C); a Rush 2049 song is queued on the 2049 player and the call turned into the "music off" case, whose
// stop command starts it (src/rush2049/track2049_audio.cpp).
//
// Menu: each game's main menu song has a switch; each time the front end's song starts (command 0x0009FFFF) a random
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
#include "elements/ui_select.h"
#include "elements/ui_slider.h"
#include "elements/ui_toggle.h"

#include "options_page.h"
#include "rush2_hooks.h"
#include "music.h"
#include "track1.h"
#include "track2049.h"
#include "car_engines.h"
#include "audio2049.h"
#include "wings.h"
#include "calspeed.h"

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
    constexpr uint32_t track_songs = 0x800CC37C;    // s16[12]: index into the song table per track.
    constexpr uint32_t song_table = 0x800CC394;     // s16[8]: sequence per Rush 2 race song.
    constexpr uint32_t requested_song = 0x800F9550; // Sequence func_80061E68 last queued for the loader.
    constexpr uint32_t sequence_player = 0x800FAEB4; // ALSeqPlayer*
    constexpr int al_playing = 1;

    // Values are kept in the configs (Main Menu Music stores Rush2-Rush2049 as 0-2, then 3 for Shuffle).
    enum class Game { Rush2, Rush1, Rush2049, CalSpeed };
    // Which Rush 2049 source a song comes from.
    enum class Only { Any, N64, Disc };

    struct Song {
        Game game;
        int id;             // Rush 2: song table index. SF Rush and Rush 2049: the game's song number.
        const char* key;    // Option id suffix.
        const char* name;
        const char* plays_on;
        Only only = Only::Any;
    };

    constexpr int disc = rush2::audio2049::disc_songs;

    // In each game's own order: Rush 2's MUSIC setting, SF Rush's random table, Rush 2049's tracks (the N64's, then
    // the disc's by its track table 0x8C0BA76C: races, battles, stunts, obstacle course, then the songs no track plays).
    // N64 songs carry the name of the disc song they arrange, matched by tempo and time-aligned chroma (docs
    // rush2049_research/audio.md "Songs"); songs 4, 7 and 8 have no clear disc counterpart and keep a number.
    constexpr std::array<Song, 54> songs = {{
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
        { Game::Rush2049, 0, "r49_0", "Bassy", "Rush 2049 Marina, Battle 1", Only::N64 },
        { Game::Rush2049, 1, "r49_1", "Garage", "Rush 2049 Haight, Battle 2", Only::N64 },
        { Game::Rush2049, 4, "r49_4", "Song 3", "Rush 2049 Civic, Battle 3", Only::N64 },
        { Game::Rush2049, 2, "r49_2", "Wingey", "Rush 2049 Metro, Battle 4", Only::N64 },
        { Game::Rush2049, 3, "r49_3", "Trancey", "Rush 2049 Mission, Battle 5", Only::N64 },
        { Game::Rush2049, 7, "r49_7", "Song 6", "Rush 2049 Presidio, Battle 6", Only::N64 },
        { Game::Rush2049, 8, "r49_8", "Song 7", "Rush 2049 Stunt 1, 2, Obstacle, Battle 7", Only::N64 },
        { Game::Rush2049, 9, "r49_9", "Flier", "Rush 2049 Stunt 3, 4, Battle 8", Only::N64 },
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
        { Game::CalSpeed, 0, "cs_0", "Song 1", "Any California Speed track" },
        { Game::CalSpeed, 1, "cs_1", "Song 2", "Any California Speed track" },
        { Game::CalSpeed, 2, "cs_2", "Song 3", "Any California Speed track" },
        { Game::CalSpeed, 3, "cs_3", "Song 4", "Any California Speed track" },
        { Game::CalSpeed, 4, "cs_4", "Song 5", "Any California Speed track" },
        { Game::CalSpeed, 5, "cs_5", "Song 6", "Any California Speed track" },
        { Game::CalSpeed, 6, "cs_6", "Song 7", "Any California Speed track" },
        { Game::CalSpeed, 7, "cs_7", "Song 8", "Any California Speed track" },
        { Game::CalSpeed, 8, "cs_8", "Song 9", "Any California Speed track" },
    }};
    constexpr int song_count = (int)songs.size();
    constexpr int first_rush1 = 8;      // Catalog index of SF Rush's first song.

    const std::string tab_id = "rush2_sound";
    enum class Mode : uint32_t { Original, ShuffleByGame, ShuffleAll };

    std::string song_option_id(const Song& s) {
        return std::string("song_") + s.key;
    }

    constexpr Game games[4] = { Game::Rush2, Game::Rush1, Game::Rush2049, Game::CalSpeed };
    constexpr int game_count = 4;

    std::string mode_option_id(Game g) {
        return g == Game::Rush2 ? "race_music_r2" : g == Game::Rush1 ? "race_music_r1"
             : g == Game::Rush2049 ? "race_music_r49" : "race_music_cs";
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
        return g == Game::Rush2 ? "games_r2" : g == Game::Rush1 ? "games_r1" : g == Game::Rush2049 ? "games_r49" : "games_cs";
    }

    std::string shuffle_all_option_id(Game g) {
        return g == Game::Rush2 ? "shuffle_all_r2" : g == Game::Rush1 ? "shuffle_all_r1"
             : g == Game::Rush2049 ? "shuffle_all_r49" : "shuffle_all_cs";
    }

    const char* game_name(Game g) {
        switch (g) {
            case Game::Rush2: return "Rush 2";
            case Game::Rush1: return "SF Rush";
            case Game::Rush2049: return "Rush 2049";
            default: return "California Speed";
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

    // Rush 2's front-end song: sequence 9, started by command 0x0009FFFF (func_800ABE7C and func_800AD07C's state 0)
    // [V]. SF Rush's front end (screen 0, func_800B9508) plays its sequence 11 and Rush 2049's menu loop
    // (func_800D71D0) its song 6 [inferred from the callers in docs/rush1_research.md section 9 and
    // docs/rush2049_research/audio.md section 3: neither is a race song]; a Dreamcast disc plays song 6 as its
    // Select.rom. California Speed's menu song isn't identified, so it has none.
    constexpr uint32_t menu_command = 0x0009FFFF;
    constexpr int rush1_menu_song = 11;
    constexpr int rush2049_menu_song = 6;
    constexpr int disc_menu_song = disc + 19;   // Select.rom, the disc's menu song.

    // The main menu songs, each with its switch (sound.json "menu_song_<key>"). Rush 2's is on by default.
    struct MenuSong {
        Game game;
        const char* key;
    };
    constexpr std::array<MenuSong, 3> menu_songs = {{
        { Game::Rush2, "r2" },
        { Game::Rush1, "r1" },
        { Game::Rush2049, "r49" },
    }};
    constexpr int menu_count = (int)menu_songs.size();
    std::array<std::atomic_bool, menu_count> menu_enabled{};
    int last_menu = -1;                     // The menu song played last (under choice_mutex).
    std::atomic<int> menu_current = -1;     // The menu song the menu plays now, -1 for Rush 2's with none on.

    std::string menu_option_id(int m) {
        return std::string("menu_song_") + menu_songs[m].key;
    }

    // Preview ids: race songs by catalog index, then the menu songs.
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

    bool game_available(Game g) {
        switch (g) {
            case Game::Rush2: return true;
            case Game::Rush1: return rush2::track1::rom_available();
            case Game::CalSpeed: return rush2::calspeed::rom_available();
            default: return rush2::wings::rom_available();
        }
    }

    // Whether Rush 2049 comes from a Dreamcast disc.
    bool disc_source() {
        auto rom = rush2::wings::get_rom();
        return rom != nullptr && rom->is_dreamcast();
    }

    // Rush 2049 Soundtrack (shown while both the N64 ROM and the disc are installed): whose Rush 2049 songs play.
    // Source follows the Games tab's Rush 2049 Source, as before the option existed.
    enum class Soundtrack : uint32_t { Source, N64, Dreamcast, Both };
    constexpr const char* soundtrack_option_id = "soundtrack_r49";
    std::atomic<uint32_t> soundtrack = (uint32_t)Soundtrack::Source;
    const std::string soundtrack_description =
        "Which Rush 2049 songs play. <recomp-color primary>Both</recomp-color> mixes the N64's and the Dreamcast's: "
        "Original plays the track's N64 or Dreamcast song, Shuffle any song of either, and the main menu either "
        "theme. <recomp-color primary>Same as Source</recomp-color> follows Rush 2049 Source in the Games tab. "
        "Applies from the next song.";

    // Whether the N64's and the disc's Rush 2049 songs may play.
    void soundtracks(bool& n64, bool& disc) {
        bool disc_now = disc_source();
        Soundtrack t = rush2::wings::both_sources() ? (Soundtrack)soundtrack.load() : Soundtrack::Source;
        n64 = t == Soundtrack::N64 || t == Soundtrack::Both || (t == Soundtrack::Source && !disc_now);
        disc = t == Soundtrack::Dreamcast || t == Soundtrack::Both || (t == Soundtrack::Source && disc_now);
    }

    // Whether the song is of a Rush 2049 soundtrack in use, so it's listed at all.
    bool song_listed(const Song& s, bool n64, bool disc) {
        return s.only == Only::Any || (s.only == Only::Disc ? disc : n64);
    }

    // Whether a race can play the song now (the 2049 sound banks load in the background).
    bool playable(int i) {
        const Song& s = songs[i];
        if (!enabled[i].load(std::memory_order_relaxed) || !game_enabled[(int)s.game].load(std::memory_order_relaxed) ||
            !game_available(s.game)) {
            return false;
        }
        if (s.only != Only::Any) {
            bool n64, disc;
            soundtracks(n64, disc);
            if (!song_listed(s, n64, disc)) {
                return false;
            }
        }
        return s.game != Game::Rush2049 || (rush2::track2049::music_ready() && rush2::audio2049::song_loaded(s.id));
    }

    int find_song(Game g, int id) {
        for (int i = 0; i < song_count; i++) {
            if (songs[i].game == g && songs[i].id == id) {
                return i;
            }
        }
        return -1;
    }

    // A random playable song (of game g, or of any game in Shuffle All when all is set, or of any game at all when
    // any is set), skipping the last one when there's another.
    int pick(bool all, Game g, bool any = false) {
        std::vector<int> pool;
        for (int i = 0; i < song_count; i++) {
            bool in = any ? true
                    : all ? in_shuffle_all[(int)songs[i].game].load(std::memory_order_relaxed) : songs[i].game == g;
            if (in && playable(i)) {
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

    // The game whose tracks the race is on, and the track's own song (catalog index, -1 if none).
    Game race_game(uint8_t* rdram, int& original) {
        int8_t t = (int8_t)MEM_B(0, (int32_t)track_id);
        bool host = t == rush2::track2049::host_slot;
        Game g = Game::Rush2;
        original = -1;
        // A Rush 2049 course: its 2049 track id, whose song Rush 2049 plays by default (audio2049::track_song).
        int course = -1;
        if (host && rush2::track2049::race_track() == rush2::track2049::obstacle) {
            course = 18;
        }
        else if (host && rush2::track2049::race_track() > 0) {
            course = rush2::track2049::race_track() - 1;
        }
        else if (t == rush2::track2049::stunt_host_slot && rush2::track2049::stunt_arena() > 0) {
            course = 14 + rush2::track2049::stunt_arena() - 1;
        }
        else if (t == rush2::track2049::stunt_host_slot && rush2::track2049::battle_arena() > 0) {
            course = 6 + rush2::track2049::battle_arena() - 1;
        }
        if (course >= 0) {
            // The course's N64 song or its disc song, whichever soundtrack is in use (with both, either one).
            g = Game::Rush2049;
            bool n64, disc;
            soundtracks(n64, disc);
            std::vector<int> own;
            for (int k : { n64 ? find_song(g, rush2::audio2049::n64_track_song(course)) : -1,
                           disc ? find_song(g, rush2::audio2049::disc_track_song(course)) : -1 }) {
                if (k >= 0 && playable(k)) {
                    own.push_back(k);
                }
            }
            if (!own.empty()) {
                original = own[std::uniform_int_distribution<size_t>(0, own.size() - 1)(rng)];
            }
            fprintf(stderr, "[Music] Rush 2049 course %d: original song %d\n", course,
                    original >= 0 ? songs[original].id : -1);
        }
        else if (host && rush2::track1::race_track() > 0) {
            g = Game::Rush1;
            original = pick(false, Game::Rush1);
        }
        else if (host && rush2::calspeed::race_track() > 0) {
            g = Game::CalSpeed;
            original = pick(false, Game::CalSpeed);
        }
        else if (rush2::track2049::mod_random_song(rdram)) {
            // A custom track without its own song ([audio] music): any song the player switched on, never its host's.
            original = pick(false, g, true);
        }
        else if (t >= 0 && t < 12) {
            original = find_song(Game::Rush2, (int16_t)MEM_H(0, (int32_t)(track_songs + t * 2)));
        }
        else {
            original = pick(false, Game::Rush2);
        }
        return g;
    }

    // The race's song (catalog index), or -1 to leave it to the game.
    int choose(uint8_t* rdram) {
        int original = -1;
        Game g = race_game(rdram, original);
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

    // Whether menu song m can play now (the 2049 sound banks load in the background).
    bool menu_playable(int m) {
        Game g = menu_songs[m].game;
        return game_available(g) && (g != Game::Rush2049 || rush2::track2049::music_ready());
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
            case Game::Rush2049: {
                // The N64's menu song or the disc's Select.rom, whichever soundtrack is in use (with both, either).
                bool n64, disc;
                soundtracks(n64, disc);
                std::vector<int> own;
                for (int id : { n64 ? rush2049_menu_song : -1, disc ? disc_menu_song : -1 }) {
                    if (id >= 0 && rush2::audio2049::song_loaded(id)) {
                        own.push_back(id);
                    }
                }
                if (own.empty()) {
                    return false;
                }
                // Not rng: this runs both under choice_mutex (menu_choice) and outside it (previews).
                thread_local std::mt19937 menu_rng{ std::random_device{}() };
                c.sequence = -1;
                c.song_2049 = own[std::uniform_int_distribution<size_t>(0, own.size() - 1)(menu_rng)];
                return true;
            }
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
        if (i >= song_count) {
            int m = i - song_count;
            MenuChoice c;
            if (!menu_song(rdram, ctx, m, c)) {
                // The 2049 banks may still be loading; anything else isn't available: give up.
                return !(menu_songs[m].game == Game::Rush2049 && game_available(Game::Rush2049));
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
            Toggle* toggle;
            Label* name;
            int id;     // Preview id: a race song's catalog index, or menu_preview(m).
        };
        struct Header {
            Element* column = nullptr;
            Toggle* toggle = nullptr;
            Select* modes = nullptr;
            Toggle* shuffle_all = nullptr;
            Element* soundtrack_row = nullptr;  // Rush 2049's Soundtrack, while both its sources are installed.
            Select* soundtrack = nullptr;
            Element* headings[2] = {};          // Rush 2049's "N64" and "Dreamcast" over their songs, with both listed.
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
        int shown_state[game_count] = { -1, -1, -1, -1 };

        static int game_state(Game g) {
            int state = (game_available(g) ? 1 : 0) | (game_enabled[(int)g].load() ? 2 : 0);
            if (g == Game::Rush2049) {
                bool n64, disc;
                soundtracks(n64, disc);
                state |= (n64 ? 4 : 0) | (disc ? 8 : 0) | (rush2::wings::both_sources() ? 16 : 0);
            }
            return state;
        }

        static Game game_of(int id) {
            return id >= song_count ? menu_songs[id - song_count].game : songs[id].game;
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

        // A game's column shows while its ROM is there. Its race songs' switches also need the game's switch on.
        void refresh_game(Game g) {
            int state = game_state(g);
            bool available = (state & 1) != 0;
            bool on = (state & 2) != 0;
            bool n64 = (state & 4) != 0;
            bool disc = (state & 8) != 0;
            Header& h = headers[(int)g];
            h.column->set_display(available ? Display::Block : Display::None);
            h.shown = available;
            if (h.soundtrack_row != nullptr) {
                h.soundtrack_row->set_display((state & 16) != 0 ? Display::Flex : Display::None);
                h.soundtrack->set_enabled(available);
                for (Element* heading : h.headings) {
                    heading->set_display(n64 && disc ? Display::Block : Display::None);
                }
            }
            for (Row& r : rows) {
                if (game_of(r.id) != g) {
                    continue;
                }
                bool menu = r.id >= song_count;
                // Rush 2049's race songs are the N64's, the disc's or both, as its Soundtrack says.
                bool listed = menu || song_listed(songs[r.id], n64, disc);
                bool active = available && (menu || on);
                r.row->set_display(listed ? Display::Flex : Display::None);
                r.button->set_enabled(available);
                r.toggle->set_enabled(active);
                r.name->set_color(active ? theme::color::Text : theme::color::TextInactive);
            }
            h.toggle->set_enabled(available);
            h.modes->set_enabled(available);
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
            arrow->set_focusable(false);
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

        // A song's row: its play button, its name and its switch (option `option_id`).
        void add_song_row(Element* column, int id, const std::string& name, const std::string& option_id,
                          std::function<void()> on_show) {
            ContextId context = get_current_context();
            Element* row = add_row(column, std::move(on_show));
            PreviewButton* button = context.create_element<PreviewButton>(row, "icons/Play.svg", ButtonStyle::Secondary, IconButtonSize::Medium);
            button->set_flex_shrink(0.0f);
            button->add_pressed_callback([id]() {
                preview_request = previewing.load() == id ? stop_request : id;
            });
            Label* label = add_name(row, name, theme::Typography::LabelMD);
            Toggle* toggle = add_switch(row, option_id);
            rows.push_back({ row, button, toggle, label, id });
        }

        void build() {
            ContextId context = get_current_context();
            recomp::config::Config& config = recompui::config::get_sound_config();

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

            for (Game g : games) {
                // The game's column: its title with the game's switch, its Race Music, In Shuffle All and menu song,
                // then its race songs.
                Element* column = add_column(g == Game::CalSpeed ? calspeed_width : game_width);
                Header& h = headers[(int)g];
                h.column = column;
                Element* title = add_title(column, game_name(g), [this, g]() {
                    show(std::string("Lets races play ") + game_name(g) + "'s songs. Switched off, each song keeps its "
                         "own switch for when it's back on.");
                });
                h.toggle = add_switch(title, game_option_id(g));

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

                if (g == Game::Rush2049) {
                    h.soundtrack_row = add_setting(column, "Soundtrack", soundtrack_description);
                    std::vector<SelectOption> choices = {
                        { "Same as Source", std::to_string((uint32_t)Soundtrack::Source) },
                        { "N64", std::to_string((uint32_t)Soundtrack::N64) },
                        { "Dreamcast", std::to_string((uint32_t)Soundtrack::Dreamcast) },
                        { "Both", std::to_string((uint32_t)Soundtrack::Both) },
                    };
                    h.soundtrack = add_select(h.soundtrack_row, choices,
                        std::to_string(std::get<uint32_t>(config.get_option_value(soundtrack_option_id))));
                    h.soundtrack->add_change_callback([](SelectOption& choice, int) {
                        recompui::config::get_sound_config().set_option_value(soundtrack_option_id,
                            (uint32_t)std::stoul(choice.value));
                    });
                }

                h.shuffle_all = add_switch(add_setting(column, "In Shuffle All", shuffle_all_description),
                                           shuffle_all_option_id(g));

                for (int m = 0; m < menu_count; m++) {
                    if (menu_songs[m].game == g) {
                        add_song_row(column, menu_preview(m), "Main Menu", menu_option_id(m),
                            [this]() { show(menu_song_description); });
                    }
                }

                for (int i = 0; i < song_count; i++) {
                    if (songs[i].game != g) {
                        continue;
                    }
                    // Rush 2049's N64 and disc songs share names: with both listed, each group gets its heading.
                    if (songs[i].only != Only::Any && (i == 0 || songs[i - 1].only != songs[i].only)) {
                        Element*& heading = h.headings[songs[i].only == Only::Disc ? 1 : 0];
                        heading = context.create_element<Label>(column,
                            songs[i].only == Only::Disc ? "Dreamcast" : "N64", theme::Typography::LabelSM);
                        heading->set_color(theme::color::TextDim);
                        heading->set_padding_top(8.0f);
                    }
                    add_song_row(column, i, songs[i].name, song_option_id(songs[i]), [this, i]() {
                        const Song& song = songs[i];
                        show(std::string("<recomp-color primary>") + song.name + "</recomp-color>. Plays on: " +
                             song.plays_on + ". Press play to hear it, or switch it off to keep it out of races.");
                    });
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
    config.add_option_change_callback(pause_music_id,
        [](recomp::config::ConfigValueVariant cur_value, recomp::config::ConfigValueVariant, recomp::config::OptionChangeContext) {
            pause_keeps_music = std::get<bool>(cur_value);
        });
    for (int m = 0; m < menu_count; m++) {
        Game g = menu_songs[m].game;
        menu_enabled[m] = g == Game::Rush2;
        config.add_bool_option(menu_option_id(m), std::string(game_name(g)) + " Menu Song", menu_song_description,
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
    config.add_enum_option(soundtrack_option_id, "Rush 2049 Soundtrack", soundtrack_description,
        std::vector<recomp::config::ConfigOptionEnumOption>{
            { Soundtrack::Source, "Source", "Same as Source" },
            { Soundtrack::N64, "N64", "N64" },
            { Soundtrack::Dreamcast, "Dreamcast", "Dreamcast" },
            { Soundtrack::Both, "Both", "Both" },
        },
        Soundtrack::Source, true);
    config.add_option_change_callback(soundtrack_option_id,
        [](recomp::config::ConfigValueVariant cur_value, recomp::config::ConfigValueVariant, recomp::config::OptionChangeContext) {
            soundtrack = std::get<uint32_t>(cur_value);
            // Starts loading the other source's songs (on a worker thread) if they're now wanted.
            rush2::track2049::music_ready();
        });
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
    pause_keeps_music = std::get<bool>(config.get_option_value(pause_music_id));
    for (int m = 0; m < menu_count; m++) {
        menu_enabled[m] = std::get<bool>(config.get_option_value(menu_option_id(m)));
    }
    for (Game g : games) {
        modes[(int)g] = std::get<uint32_t>(config.get_option_value(mode_option_id(g)));
        in_shuffle_all[(int)g] = std::get<bool>(config.get_option_value(shuffle_all_option_id(g)));
    }
    for (int i = 0; i < song_count; i++) {
        enabled[i] = std::get<bool>(config.get_option_value(song_option_id(songs[i])));
    }
    for (Game g : games) {
        game_enabled[(int)g] = std::get<bool>(config.get_option_value(game_option_id(g)));
    }
    soundtrack = std::get<uint32_t>(config.get_option_value(soundtrack_option_id));
    // Start loading Rush 2049's sound banks now, so its songs are ready for the first race.
    if (rush2::wings::rom_available()) {
        rush2::track2049::music_ready();
    }
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
    const Song& s = songs[song];
    if (s.game == Game::Rush2049) {
        rush2::track2049::queue_race_song(s.id);
        ctx->r5 = 0;
    }
    else {
        int sequence = sequence_of(rdram, ctx, song);
        if (sequence < 0) {
            race_song = -1;
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
    if (request < 0 || request >= menu_preview(menu_count)) {
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

// The pause menu's SKIP TRACK (src/pause_menu.cpp), on the game thread: another song of the race's game (or of any
// game with Shuffle All, or when the game has no other), never the one playing. A song playing in the pause (Keep
// Music Playing When Paused) changes at once through func_8008C370, as a race's start does; a stopped one becomes
// the song CONTINUE starts.
bool rush2::music::skip_song(uint8_t* rdram, recomp_context* ctx) {
    uint32_t setting = MEM_BU(0, (int32_t)music_setting);
    if (setting == 0) {
        return false; // MUSIC is off.
    }
    int song;
    {
        std::lock_guard lock{ choice_mutex };
        int original = -1;
        Game g = race_game(rdram, original);
        int current = race_song.load();
        if (current >= 0) {
            last_song = current;
        }
        song = pick((Mode)modes[(int)g].load() == Mode::ShuffleAll, g);
        if (song < 0 || song == current) {
            song = pick(true, g);
        }
        if (song < 0 || song == current) {
            return false;
        }
        last_song = song;
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
    int i = race_song.load();
    if (i < 0 || i >= song_count) {
        return false;
    }
    const Song& s = songs[i];
    out.song = s.name;
    out.game = game_name(s.game);
    // Where the song plays: Rush 2's track, Rush 2049's courses (without the game's name); SF Rush plays its songs
    // on any of its tracks.
    std::string where = s.game == Game::Rush1 || s.game == Game::CalSpeed ? "" : s.plays_on;
    if (where.rfind("Rush 2049 ", 0) == 0) {
        where = where.substr(10);
    }
    else if (where.rfind("No Rush 2049", 0) == 0) {
        where = "";
    }
    out.course = where;
    return true;
}
