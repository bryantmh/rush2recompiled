#ifndef __AUDIO2049_DC_H__
#define __AUDIO2049_DC_H__

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

#include "audio2049.h"
#include "rush2049_rom.h"

// Rush 2049's sound from a Dreamcast source (src/rush2049dc/audio2049_dc.cpp): the disc's own samples and streamed songs
// (and every other streamed song, audio2049::set_stream_song), played
// for the same calls as the N64's MusyX data. src/rush2049/audio2049.cpp hands every call here while a disc is loaded; the
// calls take N64 sound effect and song numbers, as everywhere else in the port.
namespace rush2::audio2049::dc {
    // Reads the disc's sound banks (decoding every sample) and its tables. Songs are read when they start. Without
    // effects only the song tables are read, for the disc's songs beside the N64 ROM's sound effects.
    bool load(std::shared_ptr<const rush2::rom2049::Source> source, bool effects = true);
    void unload();
    bool active();

    void set_track(int track_id);
    int track_song(int track_id);
    // The song player also plays the streamed songs (audio2049::set_stream_song), whether or not a disc is loaded:
    // play_song, stop_song, song_playing, current_song and mix's song part need no disc.
    void set_stream_song(int song, std::shared_ptr<const StreamSong> stream);
    bool stream_song_set(int song);
    void play_song(int song);
    void stop_song(float fade_seconds);
    bool song_playing();
    int current_song();

    int sfx_start(int id, float volume, float pan, float pitch, float surround);
    void sfx_update(int handle, float volume, float pan, float pitch, float surround);
    void sfx_stop(int handle);
    bool sfx_active(int handle);
    void sfx_stop_all();
    void sfx_set_engine(int handle);
    bool sfx_samples(int id, std::vector<int16_t>& pcm, uint32_t& rate);

    void mix(float* interleaved_stereo, size_t frames, uint32_t sample_rate, float music_gain, float sfx_gain,
             float engine_gain);
    void stats(Stats& out);
}

#endif
