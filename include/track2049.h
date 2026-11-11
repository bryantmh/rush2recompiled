#ifndef __TRACK2049_H__
#define __TRACK2049_H__

#include <array>
#include <cstdint>
#include <memory>
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
    // Custom tracks (src/track_mod/) take two ranges of track select ids: 38-63 between the battle arenas and California
    // Speed (64-79), then 80-248 after it. The game keeps the id in one byte (0x8010C3F0, read unsigned: us.toml turns
    // the game's lb of it into lbu), so 254 is the last; 255 stays free, as the -1 a signed read would make of it.
    // 249-254 are the Dreamcast versions of the six race tracks (disc_menu_id).
    constexpr int mod_menu_id = 38;      // Track select id of the first custom track.
    constexpr int mod_low_count = 26;    // Custom tracks in the first range (38-63).
    constexpr int mod_high_menu_id = 80; // Track select id of custom track 26, the first of the second range.
    constexpr int menu_id_limit = 255;   // Track select ids are 0-254.
    // The race tracks' Dreamcast versions (249-254, race track k at disc_menu_id + k - 1), offered beside the N64 ones
    // (12-17) when both the N64 ROM and the Dreamcast disc are stored: one track select entry per course, whose
    // version button picks which is raced (src/rush2049/track2049_menu.cpp, "Track versions"). With the disc alone,
    // 12-17 are the disc's and these aren't offered.
    constexpr int disc_menu_id = menu_id_limit - track_count;    // 249
    constexpr int mod_capacity = mod_low_count + (disc_menu_id - mod_high_menu_id);   // 195
    // Custom track `index`'s track select id (index < mod_capacity).
    constexpr int mod_id(int index) {
        return index < mod_low_count ? mod_menu_id + index : mod_high_menu_id + (index - mod_low_count);
    }
    // The custom track index of track select id t, or -1 when t isn't a custom track's id.
    constexpr int mod_index(int t) {
        if (t >= mod_menu_id && t < mod_menu_id + mod_low_count) return t - mod_menu_id;
        if (t >= mod_high_menu_id && t < disc_menu_id) return mod_low_count + (t - mod_high_menu_id);
        return -1;
    }
    constexpr bool is_mod_id(int t) {
        return mod_index(t) >= 0;
    }
    // Whether t is a race track's Dreamcast version (249-254).
    constexpr bool is_disc_course(int t) {
        return t >= disc_menu_id && t < disc_menu_id + track_count;
    }
    // The track select id of the course t is a version of: a Dreamcast version's N64 entry (12-17), else t. What the
    // unlocks, keys and logos of a course go by.
    constexpr int course_id(int t) {
        return is_disc_course(t) ? first_menu_id + (t - disc_menu_id) : t;
    }
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
        // Optional texture-load lists the blob's list calls (California Speed's dioramas), placed with the
        // container's own so the loader's linear SETTIMG scan rebases them: their bytes, the offsets of their words
        // pointing into the blob, and the offsets of the blob's words pointing into them.
        std::vector<uint8_t> loads;
        std::vector<uint32_t> load_pointers;
        std::vector<uint32_t> load_calls;
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
    // The track select's diorama tables and, as asset 3, the unlock system's shop's art: Rush 2's with the Rush 2049 and
    // SF Rush tracks' dioramas only (src/rush2049/track2049_menu.cpp shop_container; the full container doesn't fit the
    // car select scene's heap, issue #59). Call before the shop loads asset 3; the track select and circuit screen put
    // the full one back.
    void prepare_menu_art(uint8_t* rdram);
    // The name of track select entry t's diorama in the shop's art (after prepare_menu_art).
    std::string shop_diorama_name(uint8_t* rdram, int t);
    // The display list to call for a model whose own list is `model_list`, `call` being what draws it now: a list above
    // 16 MB (a custom track's diorama streamed in, src/rush2049/track2049_menu.cpp stream_diorama) is called with RT64's
    // extended addresses on (gEXSetRDRAMExtended); any other is `call`. Game thread, from the model draw.
    uint32_t extended_model_list(uint8_t* rdram, uint32_t model_list, uint32_t call);
    // Builds the track select's art container (asset 3 with the added tracks' dioramas and logos) ahead of its next
    // visit, off the game thread (the background builds, src/prewarm.cpp); that visit then only installs it.
    void prepare_menu_container();
    // A mod's tracks changed under the same mod id (src/arcade2049 reinstalled one): the track select reads them again.
    void mods_changed();
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
    // Custom track `index`'s logo ([menu] logo) in the logo container of the slot it races in (asset `container`,
    // 4 + HAWAII's or STUNT1's), so its race never shows the host's. False if it has none.
    bool mod_race_logo(uint8_t* rdram, int index, int container, std::vector<uint8_t>& out);
    // Writes that logo's texels and palette over the loaded logo asset `asset` (4-16), which the game's widgets then
    // draw (the circuit review's card, the records screen). False if the asset isn't loaded or t has no logo.
    bool write_menu_logo(uint8_t* rdram, recomp_context* ctx, int asset, int t);
    // Writes texels (as stored, width x height bytes: CI8) and a 256-color palette (512 bytes) over texture `name`
    // of loaded asset `asset` and its palette of the same name; empty texels write the asset's own back. False if the
    // asset isn't loaded or has no such CI8 texture.
    bool write_menu_texture(uint8_t* rdram, recomp_context* ctx, int asset, const std::string& name,
                            const std::vector<uint8_t>& texels, const std::vector<uint8_t>& palette);
    // Rush 2049's silver or gold coin (the face of file 68's coin model) as a 32 x 32 CI8 icon of two 16 x 32 frames
    // like Rush 2's KEYICON: left a flat dark silhouette (not found), right the coin (found); rows bottom up, palette
    // RGBA5551 big endian. For the records screen.
    bool coin_icon(const rush2::rom2049::Source& rom2049, bool gold, std::vector<uint8_t>& texels,
                   std::vector<uint8_t>& palette);

    // Converts every Rush 2049 track and arena (the race tracks' Dreamcast versions too, when they are offered), and
    // every California Speed track (as its scenery options are set), into the disk cache (those not there yet), when
    // their options are on. Any thread (src/prewarm.cpp, at launch).
    void prepare();
    // Reads the cache entry of the 2049 or California Speed track that track select id t races into memory now, for
    // the race about to load it (src/prewarm.cpp, from the car select). Any thread.
    void preload(int t);

    // The 2049 track (1-6, or obstacle) raced in the host slot, or 0 (src/rush2049/track2049.cpp). disc: a race track's
    // Dreamcast version (track select 249-254), read from the disc whichever source the Games tab uses.
    int race_track();
    void set_race_track(int k, bool disc = false);
    // Whether race_track() is a race track's Dreamcast version (track select 249-254).
    bool race_track_disc();
    // Where the race tracks' versions come from. The N64 versions (12-17) are the N64 ROM's when it is stored and
    // enabled (the Games tab's Enabled Source), else the disc's; the stunt arenas, obstacle course and battle arenas
    // are the source the game uses (rush2::wings::get_rom: Primary Source).
    bool courses_from_disc();
    // Whether the Dreamcast versions (249-254) can be offered: the Rush 2049 tracks are on and both the N64 ROM and
    // the disc are stored and enabled (Enabled Source: Both).
    bool disc_courses_available();
    // The Rush 2049 data the raced (or last raced) 2049 track was converted from, else the source in use. For the
    // race's objects (props, moving objects), which read the game's own tables of that copy.
    std::shared_ptr<const rush2::rom2049::Source> race_source();
    // The 2049 stunt arena (1-4) being played, or 0.
    int stunt_arena();
    void set_stunt_arena(int n);
    // The 2049 battle arena (1-8) being played, or 0. It is hosted in the stunt slot, so at most one of this and
    // stunt_arena() is set.
    int battle_arena();
    void set_battle_arena(int n);
    // The Rush 2 slot the raced 2049 track or stunt arena is loaded in (host_slot or stunt_host_slot), or -1.
    int loaded_slot();
    // Whether the race is on a converted course (a Rush 2049 track or arena, a custom or arcade track, a California
    // Speed track) rather than a stock Rush 2 one: one is applied in the slot being raced.
    bool converted_race(uint8_t* rdram);
    // Whether that converted course is a night one: its fog color is near black (Car Lights, src/car_lights.cpp).
    bool night_race(uint8_t* rdram);
    // Whether the race is on a custom track whose track.toml asks for California Speed's road hold
    // ([track] road_hold = true; src/calspeed/calspeed_rom.cpp rush2_calspeed_road_hold).
    bool mod_road_hold(uint8_t* rdram);
    // Whether the race is on a custom track with no [audio] music, which plays a random one of the songs the player
    // switched on (src/music.cpp).
    bool mod_random_song(uint8_t* rdram);
    // The custom track (rush2::track_mod::tracks() index) applied in the slot being raced, or -1 (src/music.cpp: its
    // own songs and the course it is a version of).
    int raced_mod(uint8_t* rdram);
    // Whether the race is on a custom track that races with smooth collision (its track.toml smooth_collision), its
    // fold angle (degrees) and whether it asks for the averaged road height (smooth_height).
    bool mod_smooth_collision(uint8_t* rdram, float* fold = nullptr, bool* height = nullptr);
    // Whether the race is on a point-to-point custom track ([track] point_to_point = true).
    bool mod_point_to_point(uint8_t* rdram);
    // The raced custom track's [look] car_light as 0xRRGGBBFF (every car drawn multiplied by it), 0 for none.
    uint32_t mod_car_light(uint8_t* rdram);
    // The factor the raced custom track's [handling] traction gives a car going `mph`, 1 without one.
    float mod_traction(uint8_t* rdram, float mph);
    // The raced custom track's [handling] air_gravity (gravity's multiple with all four wheels off the road), 1 without one.
    float mod_air_gravity(uint8_t* rdram);
    // Whether the raced custom track's [track] stunts asks for the stunt panel and scores (as Stunts on All Tracks).
    bool mod_stunts(uint8_t* rdram);
    // Whether the race is on the obstacle course.
    bool obstacle_race(uint8_t* rdram);
    // Whether the race is in a battle arena (also true at race setup, before the menu id becomes the host slot's).
    bool battle_race(uint8_t* rdram);
    // Whether a battle arena (Rush 2049's or a custom one) is hosted in the stunt slot now.
    bool battle_arena_race(uint8_t* rdram);

    // Rush 2049's game type (its 0x8014A110, set by its menus; 2049's timer setup at 0x800FC0FC tells them apart:
    // 1800 s, the stunt MINUTES option, 300 s, the battle time option) for the 2049 course hosted by the current
    // race, or none. Rush 2049 changes its rules by this type, so the hooks that port those rules ask for it rather
    // than for a course: battle arenas, once added, get theirs by returning battle.
    enum class GameType : int { none = -1, race = 0, stunt = 4, obstacle = 5, battle = 6 };
    GameType game_type(uint8_t* rdram);
    // Rules that follow from it, as Rush 2049 applies them:
    // - no reset of a car that stays slow for 6 s (0x800CF830: types 4-6); falling below y -190 still resets it;
    bool stuck_reset_off(uint8_t* rdram);
    // - a wrecked car is put back after 2.25 s in a battle (type 6) where Rush 2 and 2049's other types wait 3.5 s
    //   (2049 func_800D138C at 0x800D1454); returns the wait in seconds;
    float wreck_wait(uint8_t* rdram);
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
    // tube colored as race track `color`'s (1-6), and behind it `picture` (picture_w (at most route_picture_max_w) x
    // picture_h RGBA, rows top-down; empty for none) in place of 2049's round screenshot, at its full texel size, shown
    // as large as fits route_picture_w x route_picture_h of the track select's 320 x 240 screen (between the carousel
    // arrows). Texels, vertices and the list (at `list`) in `blob`, `pointers` the offsets of
    // the words in it pointing into it (to rebase), radius in world units. Custom tracks without an authored diorama
    // show it (src/track_mod/track_mod.cpp); the track select poses and animates it like the 2049 tracks'.
    bool route_preview(const std::vector<uint8_t>& path, int color, const std::vector<uint8_t>& picture, int picture_w,
                       int picture_h, std::vector<uint8_t>& blob, uint32_t& list, std::vector<uint32_t>& pointers,
                       float& radius);
    constexpr float route_picture_w = 224.0f, route_picture_h = 128.0f;   // the arrows are at x 21-41 and 279-299
    constexpr int route_picture_max_w = 512, route_picture_max_h = 256;   // texels
    // Track versions (docs/menus.md#track-versions): the race tracks' previews have a thin ring around the round
    // screenshot in their version's color, so the N64, Dreamcast and arcade versions of a course tell apart at a
    // glance (0xRRGGBB). The ring is drawn after the screenshot, so the track select's turn and the circuit screen
    // (which skips the screenshot) leave it alone.
    constexpr uint32_t version_ring_n64 = 0x2E7DFF;       // blue
    constexpr uint32_t version_ring_dc = 0xFF8A1E;        // orange
    constexpr uint32_t version_ring_arcade = 0xE51E2A;    // red
    constexpr uint32_t version_ring_snow = 0xE6F0FF;      // ice white: the PlayStation SF Rush's SNOW versions
    constexpr uint32_t version_ring_night = 0x7C4DFF;     // violet: its NIGHT versions
    constexpr uint32_t version_ring_stunt = 0x3CC85A;     // green: its STUNT version (Golden Gate's ramps)
    // The ring's commands sit between G_NOOPs with these words, so the track select can branch over them: it shows
    // the ring only when the course has another version (the X prompt names the version).
    constexpr uint32_t version_ring_start = 0x52494E47;   // "RING"
    constexpr uint32_t version_ring_end = 0x52494E45;     // "RINE"
    // Race track k's (1-6) preview as menu_preview_name's: `rom`'s route tube in the course's color, then the round
    // screenshot with a `ring` colored ring. The screenshot is `rom`'s own thumbnail, or with `picture_of` the one in
    // that route_preview blob (list at `picture_list`): the arcade versions' pictures with the 2049 course's route.
    // Out as route_preview's.
    bool course_preview(const rush2::rom2049::Source& rom, int k, const std::vector<uint8_t>* picture_of,
                        uint32_t picture_list, uint32_t ring, std::vector<uint8_t>& blob, uint32_t& list,
                        std::vector<uint32_t>& pointers, float& radius);
    // Whether custom track t (a mod id) is one of the app's Rush 2049 arcade mod's (rush2::arcade2049::track_mod_id,
    // matched by the mod's id so other mods can't claim it): the game filter files those under Rush 2049.
    bool mod_from_2049(int t);
    // The same for the app's SF Rush arcade mod (rush2::arcade2049::sfrush_track_mod_id): filed under SF Rush.
    bool mod_from_sfrush(int t);
    // The versions of the course track select id t is a version of (docs/menus.md#track-versions), as the track
    // select's version button goes through them: the course first (12-17: the Rush 2049 race tracks' N64 version),
    // then the Dreamcast version (249-254), then the custom tracks that are versions of it, in mod order. {t} for a
    // course with no other version. Game thread.
    std::vector<int> course_versions(int t);
    // The Dreamcast version's (249-254) preview of race track k, beside the N64 one: "R49PDTRACKk".
    std::string disc_preview_name(int k);
    // The circuit screen shows the route turning in a small window without the round picture, so the race tracks'
    // dioramas (R49TRACKn) and previews also have a flat ring in the version's color around the route, between
    // G_NOOPs with these words: shown there for a course with another version, branched over on the track select.
    constexpr uint32_t circuit_ring_start = 0x52494E43;   // "RINC"
    constexpr uint32_t circuit_ring_end = 0x52494E44;     // "RIND"
    // The Dreamcast version's diorama of race track k (the circuit screen's, the unlock shop's): "R49DTRACKk", the
    // disc's route tube with the ring in `ring`'s color, as a model for extend_menu_container (out as route_preview's).
    std::string disc_model_name(int k);
    bool course_diorama(const rush2::rom2049::Source& rom, int k, uint32_t ring, std::vector<uint8_t>& blob,
                        uint32_t& list, std::vector<uint32_t>& pointers, float& radius);
    constexpr int preview_tube_rings = 100;
    // A preview's list is followed by room for the track select to turn its model (the tube or the outline) in, so a
    // frame the renderer is still reading isn't rewritten under it (src/rush2049/track2049_menu.cpp, PreviewModel):
    // after the list's end, {u32 preview_buffer_tag, u32 the model's commands (the list's first), u32 its vertices
    // (the first in memory), u32 0}, then per spare buffer 16 bytes per vertex and 8 per command and two more.
    constexpr uint32_t preview_buffer_tag = 0x52343942;    // "R49B"
    // The same tag on a preview whose spare buffers aren't in the container: the custom tracks', whose room the track
    // select's container drops (up to 201 of them would fill the game heap) and lends from a pool while on screen.
    constexpr uint32_t preview_pooled_tag = 0x52343950;    // "R49P"
    constexpr int preview_spare_buffers = 2;
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
    // names (k = convert_track's k; 0 for a custom track, whose files are the converter's mover_paths and
    // mover_collision, with a type made for each path); reset_movers sets the objects up again on the race's next
    // physics tick.
    void set_mover_data(int k, const std::vector<uint8_t>& geometry_2049, const std::vector<uint8_t>& collision_2049,
                        const std::vector<PathRecord>& records, const std::vector<SpinRecord>& spin_records,
                        const std::vector<std::string>& model_names);
    void reset_movers();
    // Rush 2049's car lighting (2049 func_80093B20, docs/rush2049_research/collision.md#car-lighting): the colour
    // (RGBA, alpha 0xFF) race car `car` is lit with from the road under its wheels, false when no 2049 track is raced.
    bool car_light(int car, uint32_t& rgba);
    // How much of car_light's colour the track's artists painted there (0-1: the wheels on polygons with a colour
    // index other than 0, the plain one); 0 when no 2049 track is raced.
    float car_light_painted(int car);
    // The raced 2049 track's k (convert_track) when it has car lighting colours, else 0.
    int car_light_track();

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

    // The Start Game menu's choices (src/rush2049/track2049_menu.cpp, its rows top to bottom: some are only there
    // with the Rush 2049 tracks or the unlock system, so a row's place differs by machine).
    enum class StartKind : int { OneRace, Circuit, Practice, Ghost, Stunt, Battle, Records, Unlocks, Setup, None = -1 };
    // What row `row` of the Start Game menu is, and the row of a choice (-1 if this machine's menu hasn't it).
    StartKind start_menu_kind(int row);
    int start_menu_row(StartKind kind);
    // Online lobbies (src/online/online_lobby.cpp): while on, the track select offers every track this machine has,
    // whatever its profile unlocked, so the host's choice is on it.
    void set_all_tracks_open(bool on);
    // The tracks the track select offers now (its carousel, in display order): the host's, sent to the lobby.
    std::vector<int> offered_tracks(uint8_t* rdram);
    // With every track open, a joiner's track select offers only `tracks`, the host's offered_tracks (empty: all), so
    // the host's unlocks and game filter apply and both carousels step through the same entries. A change while the
    // track select is up rebuilds it.
    void set_online_offered(uint8_t* rdram, const std::vector<int>& tracks);
    // What the Start Game menu last chose (None before any choice). Leaving the menu pushes its row onto the front
    // end's row stack and zeroes the row (0x80023064), so past Start Game the row no longer tells.
    StartKind chosen_start_kind();
    // The track select's carousel entry showing track `t`, or -1 (the carousel is built as the screen opens).
    int carousel_index(uint8_t* rdram, int t);
    // Online play's name for track select id `t`: a custom track's rush2::track_mod::identity, which machines with
    // other mods enabled number differently, or "" for the other tracks (the same id on every machine).
    std::string online_track_key(int t);
    // The track select id of the custom track with that key on this machine, or -1 if it has none that loads.
    int track_for_online_key(const std::string& key);
    // A custom track's name (its track.toml's, else its folder), or "" for the other tracks.
    std::string mod_track_name(int t);
    // The track select id of a mod's custom track (its folder), or -1 while it isn't installed. Any thread.
    int mod_track_id(const std::string& mod_id, const std::string& folder);
}

#endif
