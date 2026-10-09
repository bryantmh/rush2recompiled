// Replacement asset files and the relocated game heap.
//
// Every asset is decompressed by func_80077F20 (deflate) or func_80077F84 (LZ). Both get the asset's ROM offset
// from the table at 0x800C185C and a destination, and return the decompressed size. To replace an asset, its table
// entry is pointed at a fake ROM offset that no real asset uses, and hooks at the start of both functions copy the
// replacement to the destination instead. The asset's entry in the size table 0x8001CD64 is set as well, since the
// loaders reserve heap space from it.
//
// The game heap (func_80083A0C) is a two-ended bump allocator: low allocations grow up from a base and high ones down
// from a top. The game puts it between its framebuffers (base about 0x80170CC0) and 0x803DA800, about 2.3 MB, and
// lowers the top to 0x803AA800 while the menu overlay is resident there. Rush 2049's tracks are up to 0.6 MB bigger
// than Rush 2's, and the track select's dioramas of them are real miniatures of their geometry (about 3.75 MB), so the
// heap is moved to 0x80400000-0x80B00000: past the 4 MB of the original memory map, below the interpolation buffers
// at 0x80B00000, and within the RSP's 24-bit addresses. us.toml patches func_8009FB40 and func_800A62C0 to reset the
// top to 0x80B00000 instead of 0x803AA800/0x803DA800; the overlay no longer overlaps the
// heap, so loading it doesn't need to lower the top.

#include <mutex>
#include <unordered_map>
#include <vector>

#include "miniz.h"

#include "recomp.h"
#include "librecomp/game.hpp"
#include "rush2_hooks.h"
#include "assets.h"
#include "car2049.h"
#include "track2049_convert.h"

namespace {
    // The asset tables (0x800C185C ROM offsets, 0x8001CD64 sizes, 0x71 entries) moved to copies with room for the
    // Rush 2049 cars (src/rush2049/car2049.cpp, us.toml).
    constexpr uint32_t asset_offsets = 0x80222A00;
    constexpr uint32_t asset_sizes = 0x80222C00;
    constexpr uint32_t fake_rom_base = 0x40000000;

    constexpr uint32_t heap_low = 0x8010C438;
    constexpr uint32_t heap_high = 0x8010C454;
    constexpr uint32_t heap_base = 0x8010C44C;
    constexpr uint32_t heap_mark = 0x8010C444;
    constexpr uint32_t heap_top = 0x8010C470;
    constexpr uint32_t new_heap_base = 0x80400000;
    constexpr uint32_t new_heap_top = 0x80B00000;

    struct Replacement {
        uint32_t rom;
        uint32_t size;
        std::vector<uint8_t> data;
    };

    std::mutex assets_mutex;
    std::unordered_map<int, Replacement> replaced; // By asset index; holds the original offset and size.
    std::unordered_map<uint32_t, int> by_fake_rom;
}

void rush2::assets::replace(uint8_t* rdram, int index, std::vector<uint8_t> data) {
    std::lock_guard lock{ assets_mutex };
    auto it = replaced.find(index);
    if (it == replaced.end()) {
        Replacement r;
        r.rom = (uint32_t)MEM_W(0, (int32_t)(asset_offsets + index * 4));
        r.size = (uint32_t)MEM_W(0, (int32_t)(asset_sizes + index * 4));
        it = replaced.emplace(index, std::move(r)).first;
    }
    uint32_t fake = fake_rom_base + (uint32_t)index * 0x10;
    MEM_W(0, (int32_t)(asset_offsets + index * 4)) = fake;
    MEM_W(0, (int32_t)(asset_sizes + index * 4)) = (uint32_t)data.size();
    it->second.data = std::move(data);
    by_fake_rom[fake] = index;
}

void rush2::assets::restore(uint8_t* rdram, int index) {
    std::lock_guard lock{ assets_mutex };
    auto it = replaced.find(index);
    if (it == replaced.end()) {
        return;
    }
    MEM_W(0, (int32_t)(asset_offsets + index * 4)) = it->second.rom;
    MEM_W(0, (int32_t)(asset_sizes + index * 4)) = it->second.size;
    by_fake_rom.erase(fake_rom_base + (uint32_t)index * 0x10);
    replaced.erase(it);
}

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

bool rush2::assets::read_original(uint8_t* rdram, int index, std::vector<uint8_t>& out) {
    uint32_t rom;
    {
        std::lock_guard lock{ assets_mutex };
        auto it = replaced.find(index);
        rom = it != replaced.end() ? it->second.rom : (uint32_t)MEM_W(0, (int32_t)(asset_offsets + index * 4));
    }
    auto full = recomp::get_rom();
    if (rom >= full.size()) {
        return false;
    }
    // Rush 2's LZ files (func_80077FE0's list): 0x12, 0x13, 0x15-0x18 and the cars 0x1D-0x32.
    bool lz = index == 0x12 || index == 0x13 || (index >= 0x15 && index <= 0x18) || (index >= 0x1D && index <= 0x32);
    if (lz) {
        return rush2::track2049::rush2_lz_decompress(full.data() + rom, full.size() - rom, out);
    }
    return inflate_raw(full.data() + rom, full.size() - rom, out);
}

bool rush2::assets::is_replaced(int index) {
    std::lock_guard lock{ assets_mutex };
    return replaced.contains(index);
}

// Start of func_80077F20 / func_80077F84: $a0 = ROM offset, $a1 = destination. Returns true (with $v0 = size) if
// the asset was served from a replacement.
extern "C" int rush2_asset_decompress(uint8_t* rdram, recomp_context* ctx) {
    uint32_t rom = (uint32_t)ctx->r4;
    if (rom < fake_rom_base) {
        return 0;
    }
    std::lock_guard lock{ assets_mutex };
    auto fake = by_fake_rom.find(rom);
    if (fake == by_fake_rom.end()) {
        return 0;
    }
    const std::vector<uint8_t>& data = replaced[fake->second].data;
    int32_t dest = (int32_t)ctx->r5;
    for (size_t i = 0; i < data.size(); i++) {
        MEM_B(0, dest + (int32_t)i) = data[i];
    }
    ctx->r2 = (int32_t)data.size();
    return 1;
}

// End of func_800AF3B0, which lays out memory at boot.
extern "C" void rush2_heap_init(uint8_t* rdram, recomp_context* ctx) {
    MEM_W(0, (int32_t)heap_low) = new_heap_base;
    MEM_W(0, (int32_t)heap_base) = new_heap_base;
    MEM_W(0, (int32_t)heap_mark) = new_heap_base;
    MEM_W(0, (int32_t)heap_high) = new_heap_top;
    MEM_W(0, (int32_t)heap_top) = new_heap_top;
    // The per-car-type tables move to 35-entry copies at the same time (src/rush2049/car2049.cpp).
    rush2::car2049::init_tables(rdram);
    rush2::car2049::init_assets(rdram);
    rush2::car2049::init_physics(rdram);
}
