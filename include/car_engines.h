#ifndef __CAR_ENGINES_H__
#define __CAR_ENGINES_H__

#include <cstddef>
#include <cstdint>

// Engine sounds of the cars the local players don't drive (src/car_engines.cpp): computer cars, heard by position.
namespace rush2::car_engines {
    // The "Other Cars' Engines" option (Sound tab, default on).
    void set_enabled(bool enabled);
    bool enabled();
    // Once per race frame (game thread): starts, places and tunes the other cars' engines.
    void update(uint8_t* rdram);
    // Pause, race end: stops them all (the next update starts them again).
    void stop();
    // Adds the Rush 2 cars' engines to the game's output (audio thread): interleaved stereo at the game's rate, N64
    // sample units times `scale`.
    void mix(float* samples, size_t sample_count, uint32_t sample_rate, float scale);
}

#endif
