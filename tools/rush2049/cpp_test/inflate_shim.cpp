// rush2::assets::inflate_raw for the standalone harness: src/assets.cpp also holds the game's asset hooks, which need
// the recomp runtime. Same code as there.

#include "miniz.h"

#include "assets.h"

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
