#pragma once
// Checkpoint times for the tracks Rush 2 didn't come with (src/race_time_model.cpp,
// docs/rush2049_research/checkpoints.md#09-one-rule-the-race-time-model).

#include <array>
#include <cstdint>
#include <vector>

#include "recomp.h"
#include "track2049_convert.h"

namespace rush2::race_time {
    // Per AI lane 0 point of a Rush 2 format path file (big-endian, as the assets hold it), the seconds Rush 2's
    // average car needs to drive there from the lane's first point flat out, within its top speed, acceleration,
    // cornering and braking; one more entry is the whole lane (round to its first point on a circuit). Empty if the
    // file can't be read.
    // traction: a custom track's [handling] traction steps ([mph, factor], src/track_traction.cpp): the car's
    // acceleration, cornering and braking at a speed are times the factor of the last step it reached (the snow).
    std::vector<float> lane_profile(const std::vector<uint8_t>& path_file,
                                    const std::vector<std::array<float, 2>>& traction = {});
    // Fills track.lane_time from its two paths (each converter calls it before the track goes in the disk cache).
    void fill(rush2::track2049::ConvertedTrack& track, const std::vector<std::array<float, 2>>& traction = {});
    // At the race's path load (func_80093048, after func_800924E4): the start time and every checkpoint's extensions
    // in the header copy at 0x8010BCE8 from `profile` (the loaded direction's), as Rush 2's own tracks give them.
    // False (nothing written) when the profile doesn't fit the loaded lane.
    bool apply(uint8_t* rdram, const std::vector<float>& profile);
}
