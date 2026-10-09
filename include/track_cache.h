#ifndef __TRACK_CACHE_H__
#define __TRACK_CACHE_H__

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "track1.h"
#include "track2049_convert.h"

// Converted SF Rush and Rush 2049 tracks kept on disk (src/track_cache.cpp), so a track is converted once rather than
// on the first race of it in every session: converting runs inside the race setup on the race's first frame and
// showed as a hitch at the race start (docs/race_start_hitch.md).
//
// One file per track in <app folder>/track_cache. An entry is used only if it was written by this same build of the
// executable for the same track, slot prefix and source data (the `source` hash), so a new build or another ROM
// converts again and replaces it.
namespace rush2::track_cache {
    // 64-bit FNV-1a style hash over 8-byte words, for the `source` hashes; chain calls through `seed`.
    uint64_t hash(const uint8_t* data, size_t size, uint64_t seed = 0xCBF29CE484222325ull);

    // SF Rush track t (0-6) with its in-race logo container.
    bool load_rush1(int t, const std::string& prefix, uint64_t source, rush2::track1::ConvertedTrack& track,
                    std::vector<uint8_t>& logo);
    void save_rush1(int t, const std::string& prefix, uint64_t source, const rush2::track1::ConvertedTrack& track,
                    const std::vector<uint8_t>& logo);

    // Rush 2049 track k (as rush2::track2049::convert_track numbers them).
    bool load_2049(int k, const std::string& prefix, uint64_t source, rush2::track2049::ConvertedTrack& track);
    void save_2049(int k, const std::string& prefix, uint64_t source, const rush2::track2049::ConvertedTrack& track);
}

#endif
