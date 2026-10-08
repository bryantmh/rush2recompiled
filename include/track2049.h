#ifndef __TRACK2049_H__
#define __TRACK2049_H__

#include <cstdint>
#include <string>
#include <vector>

#include "track2049_convert.h"

// Rush 2049 race tracks added to Rush 2 (src/track2049*.cpp).
//
// The track select offers ids 12-17 after Rush 2's 12 tracks, and Rush 2049's four stunt arenas as ids 25-28 (after
// the SF Rush tracks). During a race the game runs on a Rush 2 track id, the host slot, whose files and per-track
// table entries are swapped for the 2049 track's while it is raced. Stunt arenas are hosted by Rush 2's own stunt
// track, STUNT1, so they are played with Rush 2's stunt rules and scoring. Rush 2049's obstacle course follows them
// as id 29. It runs from a start to a finish, timed, so it is hosted like a race track (k = obstacle) as a one-lap
// race without drones, against a 5-minute clock as in Rush 2049. Its eight battle arenas (DM1-DM8) are ids 30-37, hosted
// by STUNT1 like the stunt arenas (a free-roaming arena with no drones or checkpoints) and played from the Start Game
// menu's BATTLE row. Rush 2049's battle weapons, health and scoring aren't ported yet (TODO.txt).
namespace rush2::track2049 {
    constexpr int track_count = 6;
    constexpr int first_menu_id = 12;   // Track select id of 2049 track 1.
    constexpr int host_slot = 2;        // HAWAII: no hardcoded per-track behaviour beyond its tables.
    constexpr int stunt_menu_id = 25;   // Track select id of stunt arena 1.
    constexpr int stunt_host_slot = 11; // STUNT1: Rush 2's stunt track.
    constexpr int obstacle_menu_id = 29; // Track select id of the obstacle course.
    constexpr int battle_menu_id = 30;   // Track select id of battle arena DM1 (30-37).
    constexpr float obstacle_time = 300.0f; // The obstacle course's clock in seconds, no checkpoint extensions.

    // The Rush 2049 Tracks option (Games tab). Tracks are offered when it is on and the 2049 ROM is present.
    void set_option(bool enabled);
    bool available();

    // Menu art (src/track2049_art.cpp). Builds asset 3 (the track select's diorama container) with a diorama model
    // R49TRACKn and a name logo texture R49LOGOn for each 2049 track, appended to Rush 2's own asset 3.
    bool build_menu_container(const std::vector<uint8_t>& rush2_asset3, const std::vector<uint8_t>& rom2049,
                              std::vector<uint8_t>& out);
    // The track select's 38-entry diorama tables and asset 3 with the added tracks' dioramas (src/track2049_menu.cpp),
    // for screens other than the track select that show them (the unlock system's shop): call before loading asset 3.
    void prepare_menu_art(uint8_t* rdram);
    // Track select entry t's diorama model name (in RDRAM) and scale, once prepare_menu_art ran.
    uint32_t diorama_name(uint8_t* rdram, int t);
    float diorama_scale(uint8_t* rdram, int t);
    // Rush 2's in-race logo container for the host slot (asset 4 + host), with its texture replaced by track k's logo.
    bool build_race_logo(const std::vector<uint8_t>& rush2_logo, const std::vector<uint8_t>& rom2049, int k,
                         std::vector<uint8_t>& out);

    // The 2049 track (1-6, or obstacle) raced in the host slot, or 0 (src/track2049.cpp).
    int race_track();
    void set_race_track(int k);
    // The 2049 stunt arena (1-4) being played, or 0.
    int stunt_arena();
    void set_stunt_arena(int n);
    // The 2049 battle arena (1-8) being played, or 0. It is hosted in the stunt slot, so at most one of this and
    // stunt_arena() is set.
    int battle_arena();
    void set_battle_arena(int n);
    // The Rush 2 slot the raced 2049 track or stunt arena is loaded in (host_slot or stunt_host_slot), or -1.
    int loaded_slot();
    // Whether the race is on the obstacle course.
    bool obstacle_race(uint8_t* rdram);
    // Whether the race is in a battle arena (also true at race setup, before the menu id becomes the host slot's).
    bool battle_race(uint8_t* rdram);

    // Rush 2049's game type (its 0x8014A110, set by its menus; 2049's timer setup at 0x800FC0FC tells them apart:
    // 1800 s, the stunt MINUTES option, 300 s, the battle time option) for the 2049 course hosted by the current
    // race, or none. Rush 2049 changes its rules by this type, so the hooks that port those rules ask for it rather
    // than for a course: battle arenas, once added, get theirs by returning battle.
    enum class GameType : int { none = -1, race = 0, stunt = 4, obstacle = 5, battle = 6 };
    GameType game_type(uint8_t* rdram);
    // Rules that follow from it, as Rush 2049 applies them:
    // - no reset of a car that stays slow for 6 s (0x800CF830: types 4-6); falling below y -190 still resets it;
    bool stuck_reset_off(uint8_t* rdram);
    // - a wrecked car is put back after 0.6 s instead of Rush 2's 3.5 s (0x800E5BF8: types 5 and 6);
    bool quick_respawn(uint8_t* rdram);
    // - no track map or radar (type 5: 0x80108AB0 leaves the radar off; type 6 builds its own HUD without them).
    bool no_map(uint8_t* rdram);
    // Track select names of track k's diorama model and logo texture (k as for convert_track).
    std::string menu_model_name(int k);
    std::string menu_logo_name(int k);
    // Puts the host slot's own files and table entries back (call from a game thread).
    void restore_host(uint8_t* rdram);

    // Moving objects (src/track2049_movers.cpp). set_mover_data hands over the raced track's 2049 geometry and
    // collision files, its path object and turning object placement records and the converted geometry's sorted model
    // names; reset_movers sets the objects up again on the race's next physics tick.
    void set_mover_data(const std::vector<uint8_t>& geometry_2049, const std::vector<uint8_t>& collision_2049,
                        const std::vector<PathRecord>& records, const std::vector<SpinRecord>& spin_records,
                        const std::vector<std::string>& model_names);
    void reset_movers();

    // Props a car knocks over (src/track2049_props.cpp): cones, gas pumps, rats, signs and cacti with Rush 2049's
    // reactions. set_prop_data hands over the raced track's prop records and its converted geometry (for model names
    // and radii); reset_props sets them up again on the race's next physics tick; props_tick runs once per physics
    // tick and props_car once per car and tick.
    void set_prop_data(const std::vector<PropRecord>& records, const std::vector<uint8_t>& converted_geometry);
    void reset_props();
    void props_tick(uint8_t* rdram, float dt);
    void props_car(uint8_t* rdram, uint32_t car);

    // Animated textures (src/track2049_texanim.cpp): Rush 2049's flip-books and scrolls, run on the loaded converted
    // geometry. set_texanim_data hands over the raced track's ConvertedTrack::tex_anims; texanim_reset (race setup)
    // makes the next tick find the loaded geometry again; texanim_tick runs once per physics tick of a race, with the
    // tick's dt in seconds (it does nothing unless a 2049 track is raced in the host slot).
    void set_texanim_data(const TexAnims& anims);
    void texanim_reset();
    void texanim_tick(uint8_t* rdram, float dt);

    // Rush 2049 music and object sounds (src/track2049_audio.cpp). update_object_sounds takes one entry per moving
    // object each tick, in a fixed order; mix_audio adds the 2049 audio to the game's output (interleaved stereo,
    // scale = the level of a full-scale sample).
    struct ObjectSound {
        enum Request { none, start, loop, stop } request = none;
        int ids[3] = { -1, -1, -1 };    // 2049 sound ids: start, loop, stop
        float pos[3] = {};
        float range = 0.0f;
        float speed_factor = 1.0f;      // |speed| / |max speed| x 0.5 + 0.5, clamped to 0-1 (func_800BF45C)
    };
    void update_object_sounds(uint8_t* rdram, const std::vector<ObjectSound>& sounds);
    void stop_object_sounds();
    // Plays 2049 sound effect `id` once from pos, heard from player 1's car with the emitter law and `range`.
    void play_effect(uint8_t* rdram, int id, const float pos[3], float range);
    void mix_audio(float* samples, size_t sample_count, uint32_t sample_rate, float scale);
    // 2049 sound effects are muted while nothing updates them (the game is paused). Effects other than the object
    // sounds (src/engine2049.cpp) call this each frame they run, which also refreshes the effects volume.
    void effects_running(uint8_t* rdram);
    // Whether sounds last updated at `updated_ms` (steady clock) should go quiet: the pause menu is open and nothing
    // has updated them for 100 ms, or nothing has for a second (the race is over). A hitch, like the one as a race
    // starts, doesn't silence them.
    bool sounds_paused(uint8_t* rdram, int64_t updated_ms);
    // Rush 2049 songs for src/music.cpp. music_ready starts loading the sound banks (once per ROM) and says whether
    // they are loaded. queue_race_song makes the race's next "music off" stop command start the song instead.
    // play_song_now / stop_song_now play or stop a song at once at the game's music volume; playing_song is the
    // song playing, or -1.
    bool music_ready();
    void queue_race_song(int song);
    int queued_race_song();
    void play_song_now(uint8_t* rdram, int song);
    void stop_song_now();
    int playing_song();
    // Clears the 2049 and SF Rush records of profile p (pak * 5 + record) and drops its saved block
    // (src/track2049_records.cpp).
    void clear_profile_records(uint8_t* rdram, int p);
}

#endif
