#ifndef __CAR_ENGINES_H__
#define __CAR_ENGINES_H__

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

// Engine sounds of the cars the local players don't drive (src/car_engines.cpp): computer cars, heard by position.
namespace rush2::car_engines {
    // The "Other Cars' Engines" option (Sound tab, default on).
    void set_enabled(bool enabled);
    bool enabled();
    // Once per race frame (game thread): starts, places and tunes the other cars' engines.
    void update(uint8_t* rdram);
    // Pause, race end: stops them all (the next update starts them again).
    void stop();
    // A California Speed car's own engine (0-8, docs/calspeed_research.md "Engine sounds"), which is played here for it
    // in place of Rush 2's engines, local players' included; -1 for other cars, and without California Speed's ROM.
    // `car` is a physics car (0x800F5470 + index * 0x81C).
    int calspeed_engine(uint8_t* rdram, uint32_t car);
    // Plays sound `id` of California Speed's sound effect bank once, centered, as its sound player starts an effect
    // (pitch from its table, volume from the effects volume 0x800D5774 / 40): the race announcer (src/announcer.cpp)
    // uses it. Replaces the one playing. False without California Speed's ROM or the sound.
    bool play_calspeed_sound(uint8_t* rdram, int id, float pitch);
    // Plays a mono 16-bit sample once, centered, at its own rate `hz`, at Rush 2's effects volume x `volume`: the
    // PlayStation SF Rush's sounds (src/time_sprint.cpp). `key` names it, so a sample played again isn't copied again.
    // Replaces the one-shot sound playing.
    bool play_sample(uint8_t* rdram, const std::string& key, const std::vector<int16_t>& pcm, uint32_t hz, float volume = 1.0f);
    // Adds the Rush 2 cars' engines to the game's output (audio thread): interleaved stereo at the game's rate, N64
    // sample units times `scale`.
    void mix(float* samples, size_t sample_count, uint32_t sample_rate, float scale);
}

#endif
