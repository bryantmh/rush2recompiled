#ifndef __TRACK_CACHE_H__
#define __TRACK_CACHE_H__

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "track1.h"
#include "track2049_convert.h"

// Converted SF Rush and Rush 2049 tracks kept on disk (src/track_cache.cpp), so a track is converted once rather than
// on the first race of it in every session: converting runs inside the race setup on the race's first frame and
// showed as a hitch at the race start (docs/race_start_hitch.md).
//
// One file per track in <app folder>/track_cache; converted Rush 2049 cars and part models, and every file converted
// from a Dreamcast disc, are blobs (load_blob) in the same folder. An entry is used only if it was written by this same build of the
// executable for the same track, slot prefix and source data (the `source` hash), so a new build or another ROM
// converts again and replaces it.
namespace rush2::track_cache {
    // 64-bit FNV-1a style hash over 8-byte words, for the `source` hashes; chain calls through `seed`.
    uint64_t hash(const uint8_t* data, size_t size, uint64_t seed = 0xCBF29CE484222325ull);

    // The cache folder, and the stamp of this build every entry is keyed on, for converted data kept as files of its
    // own (a Dreamcast disc's texture pack, src/rush2049_dc_pack.cpp).
    std::filesystem::path directory();
    uint64_t build();

    // SF Rush track t (0-6) with its in-race logo container.
    bool load_rush1(int t, const std::string& prefix, uint64_t source, rush2::track1::ConvertedTrack& track,
                    std::vector<uint8_t>& logo);
    void save_rush1(int t, const std::string& prefix, uint64_t source, const rush2::track1::ConvertedTrack& track,
                    const std::vector<uint8_t>& logo);

    // Opaque converted data (a car, a Dreamcast file, ...) under `name`, kept in the same folder with the same build and
    // source checks. `source` identifies what it was converted from (rush2::rom2049::Source::cache_key); 0 never hits.
    bool load_blob(const std::string& name, uint64_t source, std::vector<uint8_t>& out);
    void save_blob(const std::string& name, uint64_t source, const std::vector<uint8_t>& data);

    // Rush 2049 track k (as rush2::track2049::convert_track numbers them).
    bool load_2049(int k, const std::string& prefix, uint64_t source, rush2::track2049::ConvertedTrack& track);
    void save_2049(int k, const std::string& prefix, uint64_t source, const rush2::track2049::ConvertedTrack& track);
}

#endif
