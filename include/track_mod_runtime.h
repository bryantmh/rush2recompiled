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
    };

    // The enabled mods' tracks, in mod order (rescanned when the enabled mods change).
    std::vector<Entry> tracks();

    // The custom track (index into tracks()) raced in the host slot, or -1. RUSH2_TEST_MOD_TRACK=<n> races track n
    // whenever the host slot (HAWAII) is raced, for testing without a track select entry.
    int race_track();
    void set_race_track(int index);
    // The raced custom track's folder name (what its records and ghosts are kept under), or "" for none. Its
    // track.toml settings, for the race's records seed.
    std::string race_folder();
    bool race_settings(Settings& out);

    // Track `index` converted for the slot with track prefix `prefix`, from the disk cache when this build converted
    // the same files before. Writes the converter's report when it converts. False when it can't be raced.
    bool load(int index, const std::string& prefix, Converted& out);

    // Track `index`'s track select art, from the disk cache or converted now (which also caches the race files, so
    // the first race of it doesn't convert again). False when the track can't be converted: its report says why.
    bool menu_art(int index, MenuArt& out);

    // Has the textures of the tracks load and menu_art have seen (the enabled ones) drawn full size in place of their
    // stand-ins (rush2::derive::set_mod_textures, source "tracks"). Does nothing when that set hasn't changed. load
    // calls it; the track select calls it once after its menu_art calls.
    void publish_textures();

    // Lists each enabled mod's tracks in the Mods tab's details panel with the converter's errors and warnings
    // (recompui::set_mod_notes_provider). A track loads despite its problems unless nothing can be raced.
    void register_mod_notes();
}

#endif
