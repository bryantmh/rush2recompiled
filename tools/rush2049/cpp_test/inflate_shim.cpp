// rush2::assets::inflate_raw (and a no-op disk cache) for the standalone harness: src/assets.cpp also holds the game's asset hooks, which need
// the recomp runtime. Same code as there.

#include "miniz.h"

#include "assets.h"
#include "track_cache.h"

bool rush2::assets::inflate_raw(const uint8_t* src, size_t src_size, std::vector<uint8_t>& out) {
    size_t out_size = 0;
    void* data = tinfl_decompress_mem_to_heap(src, src_size, &out_size, 0);
    if (data == nullptr) {
        return false;
    }
    out.assign((const uint8_t*)data, (const uint8_t*)data + out_size);
    mz_free(data);
    return true;
}

// No disk cache in the harness: every file converts.
bool rush2::track_cache::load_blob(const std::string&, uint64_t, std::vector<uint8_t>&) {
    return false;
}
void rush2::track_cache::save_blob(const std::string&, uint64_t, const std::vector<uint8_t>&) {}
uint64_t rush2::track_cache::hash(const uint8_t*, size_t size, uint64_t seed) {
    return seed ^ size;
}
