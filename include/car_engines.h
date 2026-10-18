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
    // A California Speed car's own engine (0-8, docs/calspeed_research.md "Engine sounds"), which is played here for it
    // in place of Rush 2's engines, local players' included; -1 for other cars, and without California Speed's ROM.
    // `car` is a physics car (0x800F5470 + index * 0x81C).
    int calspeed_engine(uint8_t* rdram, uint32_t car);
    // Adds the Rush 2 cars' engines to the game's output (audio thread): interleaved stereo at the game's rate, N64
    // sample units times `scale`.
    void mix(float* samples, size_t sample_count, uint32_t sample_rate, float scale);
}

#endif
