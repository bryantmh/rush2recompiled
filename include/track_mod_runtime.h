#ifndef __TRACK_MOD_RUNTIME_H__
#define __TRACK_MOD_RUNTIME_H__

#include <filesystem>
#include <string>
#include <vector>

#include "track_mod.h"

// The game side of custom tracks (src/track_mod/track_mod.cpp).
namespace rush2::track_mod {
    struct Entry {
        std::string mod_id;
        std::filesystem::path mod_path;  // the mod's .rtz or folder
        std::string folder;              // the track's folder name (its id)
        std::string dir;                 // its path in the mod: "tracks/<folder>/" or the older "rush2/tracks/<folder>/"
        Settings settings;               // its track.toml (name falls back to the folder)
        bool hide_warnings = false;      // the mod's mod.json has "hide_track_warnings": true (Mods tab shows errors only)
    };

    // The enabled mods' tracks, in mod order (rescanned when the enabled mods change).
    std::vector<Entry> tracks();
    // A mod's file was replaced under the same id and name (src/arcade2049 rebuilt it): the next tracks() reads the
    // mods again, which the enabled mods and their file names alone don't show.
    void rescan();

    // The custom track (index into tracks()) raced in the host slot, or -1. RUSH2_TEST_MOD_TRACK=<n> races track n
    // whenever the host slot (HAWAII) is raced, for testing without a track select entry.
    int race_track();
    void set_race_track(int index);
    // The raced custom track's Settings::type: race_race (also for none), race_stunt or race_battle. An arena races in
    // the stunt track's slot (src/rush2049/track2049.cpp).
    enum RaceType : int { race_race = 0, race_stunt = 1, race_battle = 2 };
    int race_type();
    // Track `index`'s type (a RaceType) as of the last scan (the track select's), or race_race when there is no such
    // track.
    int type_of(int index);
    // Whether track `index` is a point-to-point race (track.toml point_to_point) / a run-in circuit (run_in), as of
    // the last scan.
    bool point_to_point(int index);
    bool run_in(int index);
    // The RaceType of a track with these settings (its track.toml type).
    int type_of(const Settings& settings);
    // The track prefix a custom track of these settings is converted with: its host slot's (STUNT1 for an arena).
    std::string host_prefix(const Settings& settings);
    // The raced custom track's folder name (what its records and ghosts are kept under), or "" for none. Its
    // track.toml settings, for the race's records seed.
    std::string race_folder();
    // Track `index`'s folder name as of the last scan (the track select's), or "" for none: no rescan, so the menus can
    // ask every frame.
    std::string folder(int index);
    // Track `index`'s identity for online play: its mod and folder and a hash of every file its conversion reads
    // ("mod/folder#hash"), so two machines race it only with the same files. "" until the track has been read (the
    // track select's menu_art reads every enabled track).
    std::string identity(int index);
    bool race_settings(Settings& out);

    // Track `index` converted for the slot with track prefix `prefix`, from the disk cache when this build converted
    // the same files before. Writes the converter's report when it converts. False when it can't be raced.
    bool load(int index, const std::string& prefix, Converted& out);

    // Track `index`'s track select art, from the disk cache or converted now (which also caches the race files, so
    // the first race of it doesn't convert again). False when the track can't be converted: its report says why.
    bool menu_art(int index, MenuArt& out);

    // Converts the enabled mods' tracks into the disk cache (those not there yet) and keeps their menu art and file
    // hashes in memory, so the track select and the race read none of the mods' files. Any thread: src/prewarm.cpp runs
    // it at launch and again whenever signature() changes.
    void prepare();
    // Changes when the enabled mods (or their files' paths) do.
    std::string signature();
    // Reads track `index`'s cache entries into memory now, for the race about to load it (src/prewarm.cpp, from the
    // car select). Does nothing for a track prepare hasn't read.
    void preload(int index);

    // Has the textures of the tracks load and menu_art have seen (the enabled ones) drawn full size in place of their
    // stand-ins (rush2::derive::set_mod_textures, source "tracks"). Does nothing when that set hasn't changed. load
    // calls it; the track select calls it once after its menu_art calls.
    void publish_textures();

    // Lists each enabled mod's tracks in the Mods tab's details panel with the converter's errors and warnings
    // (recompui::set_mod_notes_provider). A track loads despite its problems unless nothing can be raced.
    void register_mod_notes();
}

#endif
