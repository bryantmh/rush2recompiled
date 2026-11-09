#ifndef __ROCK_MUSIC_H__
#define __ROCK_MUSIC_H__

#include <filesystem>
#include <memory>

#include "stream_decode.h"

// San Francisco Rush: The Rock's music (src/arcade2049/rock_music.cpp, docs/sfrush_arcade.md#music): sequenced songs a
// small script machine plays with the samples of the arcade's four sound ROMs (audio.u62, u61, u53, u49), synthesized
// live as the arcade's sound board does, ported from ProjectR's native port of its sound program (project-r.exe).
namespace rush2::rock_music {
    // The race tracks' songs, songs[n] being TRACK(n + 1)'s (TRACK4 is The Rock), with their command numbers (the
    // sound program's command table, project-r.exe 0x1406EB6C0) and the course each plays on.
    struct SongInfo {
        int command;
        const char* name;
    };
    extern const SongInfo songs[];
    extern const int song_count;

    // Reads the sound ROMs from the arcade data folder (8 MB). False if they aren't all there. Any thread.
    bool load(const std::filesystem::path& data_dir);
    bool loaded();
    // An endless decoder playing song `n` (index into songs) from the start: frames is SIZE_MAX, 22050 Hz.
    std::unique_ptr<rush2::audio2049::StreamDecoder> open(int n);
}

#endif
