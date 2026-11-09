#ifndef __MUSIC_STREAMS_H__
#define __MUSIC_STREAMS_H__

#include <string>
#include <vector>

#include "track_mod_runtime.h"

// The songs streamed from files rather than played from a ROM (src/music_streams.cpp), registered with the song player
// (audio2049::set_stream_song) under the ids include/audio2049.h gives each source:
//   - the Rush 2049 arcade's music (the arcade data's AUDIO/*.str): a song per arcade track and its select screen's
//   - the PlayStation SF Rush disc's CD audio (the disc the sfrush_ps1 mod is built from)
//   - custom tracks' own songs (track.toml [audio] music naming .ogg or .wav files in the track's folder)
// src/music.cpp lists them and picks among them.
namespace rush2::music_streams {
    // The 2049 arcade's songs by audio2049::arcade2049_songs + n: AUDIO/track<n + 1>.str for n 0-6 (TRACK1-7),
    // therock.str (7, TRACK8's) and select.str (8, the select screens').
    constexpr int arcade_track8 = 7;
    constexpr int arcade_select = 8;
    constexpr int arcade_song_count = 9;
    // The PlayStation disc's CD audio tracks by audio2049::ps1_songs + n (the disc's tracks 2-5).
    constexpr int ps1_song_count = 4;

    // Looks for the arcade data and the PlayStation disc and registers their songs (or drops them when they're gone).
    // Reads folders and the disc's .cue: call off the game thread.
    void refresh_sources();

    // A custom track's song file.
    struct ModSong {
        int id;                    // its audio2049 song id (mod_songs and up; the same file keeps its id)
        std::string name;          // the file's name without its extension
        std::string track;         // the track's name
        std::string owner;         // "<mod id>/<folder>": the track it belongs to
    };
    // The enabled mods' song files, registered with the song player, in track order. Reads the mod list again when
    // it changed (track_mod::tracks).
    std::vector<ModSong> mod_songs();
    // Custom track `e`'s song files' ids (its [audio] music entries that name a file), registering them if needed.
    std::vector<int> track_song_ids(const rush2::track_mod::Entry& e);
    // Whether an [audio] music entry names a file (.ogg or .wav) rather than a song of the game.
    bool is_song_file(const std::string& entry);
}

#endif
