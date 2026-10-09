#ifndef __AUDIO2049_H__
#define __AUDIO2049_H__

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

#include "rush2049_rom.h"

// Rush 2049's music and sound effects, played on the host (src/audio2049.cpp): a C++ port of the MusyX sound system
// Rush 2049 uses, driven by the data in the user's Rush 2049 ROM. Pure C++; no RDRAM or recompiler dependencies.
//
// Threading: mix() runs on the audio thread; every other call may come from any thread. All calls are serialised by
// one internal mutex, so keep calls cheap and infrequent (a few per frame).
//
// Levels: with music_gain = sfx_gain = 1.0, mix() produces Rush 2049's mix at its default options (music and sound
// effects volume 10/10, which Rush 2049 turns into MusyX master volume 114/127). Voice gains match the N64; a full
// 15-bit voice gain comes out at 0.5, as the RSP mixer's own scale isn't ported (see docs/rush2049_research/audio.md
// §4). The race songs then run at -17..-23 dBFS RMS; loud passages can still pass 1.0, so clamp after mixing.
namespace rush2::audio2049 {
    constexpr int song_count = 12;   // Rush 2049 songs 0-11 (ROM files 10-21). 10 and 11 end; 0-9 loop forever.
    constexpr int sfx_count = 0x77;  // Rush 2049 sound effect ids 0x00-0x76.

    // Parses the sound data out of the big-endian Rush 2049 ROM and decodes every sample (about 50 ms). Returns
    // false if the ROM isn't readable. Stops anything playing. Safe to call again with another ROM.
    bool load(const std::vector<uint8_t>& rom);
    // Loads the sound of a Rush 2049 source: the N64 ROM's MusyX data, or a Dreamcast disc's own samples and streamed
    // songs (src/audio2049_dc.cpp), which then answer every call below in the same N64 terms.
    bool load(std::shared_ptr<const rush2::rom2049::Source> source);
    bool loaded();

    // The 2049 track id (0-18) being raced, or -1. A Dreamcast disc has its own song for every track: a song that
    // the N64 plays on this track then plays the disc's song for it.
    void set_track(int track_id);
    // A sound effect's sample (its loop, for a looping one) and rate, for code that plays it itself. Dreamcast only.
    bool sfx_samples(int id, std::vector<int16_t>& pcm, uint32_t& rate);

    // A Dreamcast disc's songs by its own song list (0x8C0BA818: 18 .STR streams, then HighScore.rom and
    // Select.rom): play_song(disc_songs + n) plays song n of the disc, which has more songs than the N64.
    constexpr int disc_songs = 100;
    constexpr int disc_song_count = 20;

    // The song Rush 2049 plays on 2049 track id t (0-5 races, 6-13 battle arenas, 14-17 stunt arenas, 18 the obstacle
    // course) with its default "per track" music option: the N64's (0x8010FFD4), or from a Dreamcast disc
    // disc_songs + its song for the track (0x8C0BA76C). -1 if there's none (the disc picks one at random).
    int track_song(int track);

    // Starts song 0-11 (or a disc's disc_songs + n) from the top, stopping the current one at once. Out-of-range ids
    // stop the music.
    void play_song(int song);
    // Stops the song; with a fade, ramps the song's volume to zero over fade_seconds first (notes keep playing their
    // releases). song_playing() stays true until the song has stopped.
    void stop_song(float fade_seconds = 0.0f);
    bool song_playing();
    int current_song(); // -1 if none

    // Starts a sound effect by Rush 2049 id the way Rush 2049's sound thread does: sndFXStart at the effect's default
    // volume and pan, then volume (0-1) as MIDI volume, pan (-1 left .. 1 right) as MIDI pan and pitch (ratio, up to
    // 2.0) as the Doppler controller. surround (-1 .. 1) is MusyX surround panning: -1 (the default) keeps the sound
    // in the front pair; toward 1 it moves into the Dolby Surround channel, which is matrixed into stereo as
    // left + S, right - S. Rush 2049 sets it from 3D emitters (see emitter_mix). Returns a handle, or -1 if the id
    // doesn't exist or no voice is free.
    int sfx_start(int id, float volume = 1.0f, float pan = 0.0f, float pitch = 1.0f, float surround = -1.0f);
    // Changes a playing effect's parameters (same meaning as sfx_start). Ignored if the handle has ended.
    void sfx_update(int handle, float volume, float pan, float pitch, float surround = -1.0f);
    // Key-off: the effect's macro runs its release (many effects stop at once, looping ones fade or stop as their
    // macro says). Ignored if the handle has ended.
    void sfx_stop(int handle);
    // True while the effect's macro runs or its sample still sounds.
    bool sfx_active(int handle);
    // Stops every effect at once (no release).
    void sfx_stop_all();

    // Adds interleaved stereo audio at sample_rate into the buffer (does not clear it). Music voices are scaled by
    // music_gain and effect voices by sfx_gain.
    void mix(float* interleaved_stereo, size_t frames, uint32_t sample_rate, float music_gain, float sfx_gain);

    // Rush 2049's 3D emitter law for one listener (func_80098AE4): the volume, pan and surround it would pass to
    // sfx_update for an emitter at `emitter` heard by a listener at `listener` (Rush 2049 uses the player car's
    // position) with the camera's back and up unit vectors (camera rows 2 and 1). range > 0: volume falls linearly
    // from max_volume at the listener to min_volume at `range` and is clamped at 0; range <= 0: max_volume. Pan and
    // surround are the direction's right and back components scaled by (1 - volume). With several listeners
    // (split screen) Rush 2049 sums the volumes (clamped to 1) and averages pan and surround weighted by volume.
    struct EmitterParams {
        float volume;
        float pan;
        float surround;
    };
    EmitterParams emitter_mix(const float emitter[3], const float listener[3], const float listener_back[3],
                              const float listener_up[3], float range, float max_volume = 1.0f,
                              float min_volume = 0.0f);

    // Diagnostics for tests and overlays.
    struct Stats {
        int song = -1;
        uint32_t song_tick = 0;       // sequencer position (384 ticks per beat)
        uint32_t song_loops = 0;      // times the song looped
        uint32_t notes_started = 0;   // since play_song
        uint32_t notes_dropped = 0;   // note-ons that found no voice
        int voices_music = 0;         // allocated synth voices
        int voices_sfx = 0;
        int voices_sounding = 0;      // sample playback active
        uint16_t channel_notes[16]{}; // notes started per MIDI channel since play_song
        uint32_t sfx_voice_gain[2]{}; // left/right voice volume (0-32767) of the newest sfx voice, before ADSR
    };
    Stats stats();
}

#endif
