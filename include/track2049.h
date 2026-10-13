#ifndef __TRACK2049_H__
#define __TRACK2049_H__

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "recomp.h"
#include "track2049_convert.h"

// Rush 2049 race tracks added to Rush 2 (src/rush2049/track2049*.cpp).
//
// The track select offers ids 12-17 after Rush 2's 12 tracks, and Rush 2049's four stunt arenas as ids 25-28 (after
// the SF Rush tracks). During a race the game runs on a Rush 2 track id, the host slot, whose files and per-track
// table entries are swapped for the 2049 track's while it is raced. Stunt arenas are hosted by Rush 2's own stunt
// track, STUNT1, so they are played with Rush 2's stunt rules and scoring. Rush 2049's obstacle course follows them
// as id 29. It runs from a start to a finish, timed, so it is hosted like a race track (k = obstacle) as a one-lap
// race without drones, against a 5-minute clock as in Rush 2049. Its eight battle arenas (DM1-DM8) are ids 30-37, hosted
// by STUNT1 like the stunt arenas (a free-roaming arena with no checkpoints; its drones are the Games tab's computer
// opponents) and played from the Start Game menu's BATTLE row, with Rush 2049's battle rules (src/rush2049/battle.cpp).
namespace rush2::track2049 {
    constexpr int track_count = 6;
    constexpr int first_menu_id = 12;   // Track select id of 2049 track 1.
    // Race track 1-6's name (index k - 1), as the Dreamcast version names them (the N64 game calls them Track 1-6).
    constexpr const char* track_names[track_count] = { "Marina", "Haight", "Civic", "Metro", "Mission", "Presidio" };
    constexpr int host_slot = 2;        // HAWAII: no hardcoded per-track behaviour beyond its tables.
    constexpr int stunt_menu_id = 25;   // Track select id of stunt arena 1.
    constexpr int stunt_host_slot = 11; // STUNT1: Rush 2's stunt track.
    constexpr int obstacle_menu_id = 29; // Track select id of the obstacle course.
    constexpr int battle_menu_id = 30;   // Track select id of battle arena DM1 (30-37).
    constexpr int mod_menu_id = 38;      // Track select id of the first custom track (src/track_mod/), up to 63.
    constexpr int mod_capacity = 26;     // Custom tracks the track select (and circuits) can offer.
    constexpr float obstacle_time = 300.0f; // The obstacle course's clock in seconds, no checkpoint extensions.

    // The Rush 2049 Tracks option (Games tab). Tracks are offered when it is on and the 2049 ROM is present.
    void set_option(bool enabled);
    bool available();

    // Menu art (src/rush2049/track2049_art.cpp). Builds asset 3 (the track select's diorama container) with a diorama model
    // R49TRACKn, a track select preview (menu_preview_name) and a name logo texture R49LOGOn for each 2049 track,
    // appended to Rush 2's own asset 3.
    bool build_menu_container(const std::vector<uint8_t>& rush2_asset3, const rush2::rom2049::Source& rom2049,
                              std::vector<uint8_t>& out);
    // A model for extend_menu_container: its texels, vertices and list in `blob` (the list at `list`), `pointers` the
    // offsets of the blob's words pointing into it, radius in world units.
    struct MenuModel {
        std::string name;
        std::vector<uint8_t> blob;
        uint32_t list = 0;
        std::vector<uint32_t> pointers;
        float radius = 0.0f;
    };
    // A logo for it: 128 x 32 CI8 texels (rows bottom-up, as Rush 2 stores its logos) and 256 RGBA5551 colors.
    struct MenuLogo {
        std::string name;
        std::vector<uint8_t> texels, palette;
    };
    // A container (asset 3, or one build_menu_container made) with these models and logos added the way
    // build_menu_container adds the 2049 tracks' (custom tracks, src/track_mod/).
    bool extend_menu_container(const std::vector<uint8_t>& asset3, const std::vector<MenuModel>& models,
                               const std::vector<MenuLogo>& logos, std::vector<uint8_t>& out);
    // An RGBA picture as CI8 (median cut; index 255 clear, for texels with alpha below 128), as the previews' are.
    void quantize_ci8(const std::vector<std::array<uint8_t, 4>>& rgba, std::vector<uint8_t>& indices,
                      std::array<uint16_t, 256>& palette);
    // The track select's 38-entry diorama tables and asset 3 with the added tracks' dioramas (src/rush2049/track2049_menu.cpp),
    // for screens other than the track select that show them (the unlock system's shop): call before loading asset 3.
    void prepare_menu_art(uint8_t* rdram);
    // Asset 3 as the track select gets it (with the added tracks' art), built here when the game hasn't yet: for the
    // catalog (rush2::catalog), which dumps every texture the game can draw whichever menus have been opened. Any
    // thread. False when no other game's tracks are offered (asset 3 is Rush 2's own).
    bool catalog_menu_container(std::vector<uint8_t>& out);
    // Track select entry t's diorama model name (in RDRAM) and scale, once prepare_menu_art ran.
    uint32_t diorama_name(uint8_t* rdram, int t);
    float diorama_scale(uint8_t* rdram, int t);
    // Gives the renderer the race tracks' banners' exact colors in place of their CI8 copies' 255
    // (src/texture_upscale.cpp, add_exact_image). Called once at startup.
    void add_banner_images();
    // Rush 2's in-race logo container for the host slot (asset 4 + host), with its texture replaced by track k's logo.
    bool build_race_logo(const std::vector<uint8_t>& rush2_logo, const rush2::rom2049::Source& rom2049, int k,
                         std::vector<uint8_t>& out);
    // A Rush 2 logo container's (asset 4 + track) 128 x 32 CI8 texels and 256-color palette: their offsets in it, and
    // the container with them replaced (texels rows bottom-up, palette RGBA5551 big endian).
    bool logo_data_offsets(const std::vector<uint8_t>& logo, uint32_t& texels, uint32_t& palette);
    bool set_race_logo(const std::vector<uint8_t>& logo, const std::vector<uint8_t>& texels,
                       const std::vector<uint8_t>& palette, std::vector<uint8_t>& out);
    // Track select entry t (a race track of any game) set up to be raced as a race would set it up: the menu track id
    // 0x8010C3F0 becomes the slot it runs in, which this returns (the race setup hook leaves it so). Game thread.
    int host_menu_track(uint8_t* rdram, int t);
    // Track select entry t's in-race logo container (asset 4 + the slot host_menu_track gives it), as the race shows it.
    bool menu_track_logo(uint8_t* rdram, int t, std::vector<uint8_t>& out);
    // Writes that logo's texels and palette over the loaded logo asset `asset` (4-16), which the game's widgets then
    // draw (the circuit review's card, the records screen). False if the asset isn't loaded or t has no logo.
    bool write_menu_logo(uint8_t* rdram, recomp_context* ctx, int asset, int t);

    // The 2049 track (1-6, or obstacle) raced in the host slot, or 0 (src/rush2049/track2049.cpp).
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
    // Rush 2049's own track select preview of track k (src/rush2049/track2049_art.cpp, docs/rush2049_research/menus.md §7.5):
    // its round screenshot with the route tube (race tracks) or the arena's outline (TRK_* of file 60) in front, as
    // one model laid out in Rush 2049's track select camera space (x right, y up, z forward, 1/16 units). A race
    // track's preview starts with the tube's preview_tube_rings x 4 vertices, ring by ring (top +side, top -side,
    // floor -side, floor +side), which the track select recolors each frame (preview_tube_color).
    std::string menu_preview_name(int k);
    // Rush 2049's track select preview (as menu_preview_name's) of AI path file `path` (a converted track's): its route
    // tube colored as race track `color`'s (1-6), and behind it `picture` (128 x 128 RGBA, rows top-down; empty for
    // none) as 2049's round screenshot. Texels, vertices and the list (at `list`) in `blob`, `pointers` the offsets of
    // the words in it pointing into it (to rebase), radius in world units. Custom tracks without an authored diorama
    // show it (src/track_mod/track_mod.cpp); the track select poses and animates it like the 2049 tracks'.
    bool route_preview(const std::vector<uint8_t>& path, int color, const std::vector<uint8_t>& picture,
                       std::vector<uint8_t>& blob, uint32_t& list, std::vector<uint32_t>& pointers, float& radius);
    constexpr int preview_tube_rings = 100;
    // 2049's track select camera, fitted to a capture of it (§7.5): focal length and center in screen pixels.
    constexpr float preview_focal = 167.0f, preview_center_x = 162.0f, preview_center_y = 116.0f;
    // A point of the displayed model's own space (2049 units: x, y up, z) to the preview's camera space (model
    // units), with the model turned by `angle` about its y axis (2049 turns the selected track once every 10 s).
    void preview_place(const float local[3], float angle, float out[3]);
    // The inverse at angle 0.
    void preview_unplace(const float placed[3], float local[3]);
    // Top color of ring `ring` of race track n's tube with the white highlight at phase (0-1) of its run along the
    // route (func_8038A820).
    void preview_tube_color(int n, int ring, float phase, uint8_t rgb[3]);
    // Puts the host slot's own files and table entries back (call from a game thread).
    void restore_host(uint8_t* rdram);

    // Moving objects (src/rush2049/track2049_movers.cpp). set_mover_data hands over the raced track's 2049 geometry and
    // collision files, its path object and turning object placement records and the converted geometry's sorted model
    // names (k = convert_track's k, 0 for a custom track); reset_movers sets the objects up again on the race's next
    // physics tick.
    void set_mover_data(int k, const std::vector<uint8_t>& geometry_2049, const std::vector<uint8_t>& collision_2049,
                        const std::vector<PathRecord>& records, const std::vector<SpinRecord>& spin_records,
                        const std::vector<std::string>& model_names);
    void reset_movers();
    // Rush 2049's car lighting (2049 func_80093B20, docs/rush2049_research/collision.md#car-lighting): the colour
    // (RGBA, alpha 0xFF) race car `car` is lit with from the road under its wheels, false when no 2049 track is raced.
    bool car_light(int car, uint32_t& rgba);

    // Props a car knocks over (src/rush2049/track2049_props.cpp): cones, gas pumps, rats, signs and cacti with Rush 2049's
    // reactions. set_prop_data hands over the raced track's prop records and its converted geometry (for model names
    // and radii); reset_props sets them up again on the race's next physics tick; props_tick runs once per physics
    // tick and props_car once per car and tick.
    void set_prop_data(const std::vector<PropRecord>& records, const std::vector<uint8_t>& converted_geometry);
    void reset_props();
    void props_tick(uint8_t* rdram, float dt);
    void props_car(uint8_t* rdram, uint32_t car);

    // Animated textures (src/rush2049/track2049_texanim.cpp): Rush 2049's flip-books and scrolls, run on the loaded converted
    // geometry. set_texanim_data hands over the raced track's ConvertedTrack::tex_anims; texanim_reset (race setup)
    // makes the next tick find the loaded geometry again; texanim_tick runs once per physics tick of a race, with the
    // tick's dt in seconds (it does nothing unless a 2049 track is raced in the host slot).
    void set_texanim_data(const TexAnims& anims);
    void texanim_reset();
    void texanim_tick(uint8_t* rdram, float dt);
    // Every race setup, a RESTART included (which reloads no track files): the moving objects, props, battle and
    // animated textures start over on the race's next physics tick.
    void reset_race_objects();

    // Rush 2049 music and object sounds (src/rush2049/track2049_audio.cpp). update_object_sounds takes one entry per moving
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
    // sounds (src/rush2049/engine2049.cpp) call this each frame they run, which also refreshes the effects volume.
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
    // (src/rush2049/track2049_records.cpp).
    void clear_profile_records(uint8_t* rdram, int p);
}

#endif
