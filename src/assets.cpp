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
// heap is moved to 0x80420000-0x80AE0000: past the 4 MB of the original memory map and the scene node table moved to
// 0x80400000 (include/scene_nodes.h; 3 MB of the heap was still free in the arcade Metro's race), below the 2D display lists at
// 0x80AE0000 (src/hud.cpp) and the interpolation buffers at 0x80B00000, and within the RSP's 24-bit addresses. us.toml patches func_8009FB40 and func_800A62C0 to reset the
// top to 0x80AE0000 instead of 0x803AA800/0x803DA800; the overlay no longer overlaps the
// heap, so loading it doesn't need to lower the top.

#include <atomic>
#include <mutex>
#include <unordered_map>
#include <vector>

#include "car_types.h"
#include "miniz.h"

#include "recomp.h"
#include "librecomp/game.hpp"
#include "rush2_hooks.h"
#include "assets.h"
#include "calspeed.h"
#include "car2049.h"
#include "paint_colors.h"
#include "car_mods.h"
#include "rush2.h"
#include "track2049_convert.h"
#include "texture_origin.h"

namespace {
    // The asset tables (0x800C185C ROM offsets, 0x8001CD64 sizes, 0x71 entries) moved to copies with room for the
    // Rush 2049 and California Speed cars (include/car_types.h, us.toml).
    constexpr uint32_t asset_offsets = rush2::car_types::asset_offsets;
    constexpr uint32_t asset_sizes = rush2::car_types::asset_sizes;
    constexpr uint32_t fake_rom_base = 0x40000000;
    constexpr int asset_slots = rush2::assets::slot_count;

    constexpr uint32_t heap_low = 0x8010C438;
    constexpr uint32_t heap_high = 0x8010C454;
    constexpr uint32_t heap_base = 0x8010C44C;
    constexpr uint32_t heap_mark = 0x8010C444;
    constexpr uint32_t heap_top = 0x8010C470;
    constexpr uint32_t new_heap_base = 0x80420000; // 0x80400000-0x80420000: the scene node table (include/scene_nodes.h)
    constexpr uint32_t new_heap_top = 0x80AE0000; // 0x80AE0000-0x80B00000: the 2D display lists (src/hud.cpp)

    struct Replacement {
        uint32_t rom;
        uint32_t size;
        std::vector<uint8_t> data;              // what the game loads: base, or base patched (rush2::assets::Patcher)
        std::vector<uint8_t> base;              // the replacement as given, or Rush 2's own bytes (own false)
        std::vector<rush2::assets::Span> spans; // the parts of base from another game than Rush 2
        bool own = true;                        // replaced by replace(); false: only patched
    };

    std::mutex assets_mutex;
    std::atomic<uint8_t*> game_rdram = nullptr; // Saved by the asset loads, for read_current.
    std::unordered_map<int, Replacement> replaced; // By asset index; holds the original offset and size.
    std::unordered_map<uint32_t, int> by_fake_rom;
    std::atomic<uint64_t> changes = 0;
    rush2::assets::Patcher patcher;

    bool is_lz(int index) {
        // Rush 2's LZ files (func_80077FE0's list): 0x12, 0x13, 0x15-0x18 and the cars 0x1D-0x32.
        return index == 0x12 || index == 0x13 || (index >= 0x15 && index <= 0x18) || (index >= 0x1D && index <= 0x32);
    }
    bool read_rom_asset(int index, uint32_t rom, std::vector<uint8_t>& out) {
        auto full = recomp::get_rom();
        if (rom == 0 || rom >= full.size()) return false;
        if (is_lz(index)) return rush2::track2049::rush2_lz_decompress(full.data() + rom, full.size() - rom, out);
        return rush2::assets::inflate_raw(full.data() + rom, full.size() - rom, out);
    }

    // Points the asset table at a replacement's data (assets_mutex held).
    void install(uint8_t* rdram, int index, const Replacement& r) {
        uint32_t fake = fake_rom_base + (uint32_t)index * 0x10;
        MEM_W(0, (int32_t)(asset_offsets + index * 4)) = fake;
        MEM_W(0, (int32_t)(asset_sizes + index * 4)) = (uint32_t)r.data.size();
        by_fake_rom[fake] = index;
    }
    // The asset table back on Rush 2's own copy (assets_mutex held).
    void uninstall(uint8_t* rdram, int index, const Replacement& r) {
        MEM_W(0, (int32_t)(asset_offsets + index * 4)) = r.rom;
        MEM_W(0, (int32_t)(asset_sizes + index * 4)) = r.size;
        by_fake_rom.erase(fake_rom_base + (uint32_t)index * 0x10);
    }
    // Sets the bytes the game gets for r: base, or base patched (assets_mutex held). True if they changed.
    bool patch(uint8_t* rdram, int index, Replacement& r) {
        std::vector<uint8_t> out;
        bool patched = patcher && patcher(rdram, index, r.base, out);
        const std::vector<uint8_t>& data = patched ? out : r.base;
        if (data == r.data) return false;
        r.data = data;
        return true;
    }
}

uint64_t rush2::assets::generation() {
    return changes;
}

uint8_t* rush2::assets::rdram() {
    return game_rdram.load();
}

void rush2::assets::replace(uint8_t* rdram, int index, std::vector<uint8_t> data, rush2::origin::Game game) {
    uint32_t size = (uint32_t)data.size();
    replace(rdram, index, std::move(data), std::vector<Span>{ { 0, size, game } });
}

void rush2::assets::replace(uint8_t* rdram, int index, std::vector<uint8_t> data, std::vector<Span> spans) {
    std::lock_guard lock{ assets_mutex };
    auto it = replaced.find(index);
    if (it == replaced.end()) {
        Replacement r;
        r.rom = (uint32_t)MEM_W(0, (int32_t)(asset_offsets + index * 4));
        r.size = (uint32_t)MEM_W(0, (int32_t)(asset_sizes + index * 4));
        it = replaced.emplace(index, std::move(r)).first;
    }
    Replacement& r = it->second;
    r.own = true;
    r.base = std::move(data);
    r.spans = std::move(spans);
    if (patch(rdram, index, r)) changes++;
    install(rdram, index, r);
}

void rush2::assets::restore(uint8_t* rdram, int index) {
    std::lock_guard lock{ assets_mutex };
    auto it = replaced.find(index);
    if (it == replaced.end()) {
        return;
    }
    // Still patched: back to Rush 2's own bytes, patched.
    Replacement& r = it->second;
    if (patcher && read_rom_asset(index, r.rom, r.base)) {
        r.own = false;
        r.spans.clear();
        patch(rdram, index, r);
        if (r.data != r.base) {
            install(rdram, index, r);
            changes++;
            return;
        }
    }
    uninstall(rdram, index, r);
    replaced.erase(it);
    changes++;
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
    return read_rom_asset(index, rom, out);
}

namespace {
    // The game inflates its main segment (code and data, raw deflate at ROM 0xAFD0C0) to 0x800539E0 at boot
    // (tools/extract.py). Before it, the first segment (ROM 0x1000, not compressed) is at 0x80000400.
    constexpr uint32_t main_rom = 0xAFD0C0, main_vram = 0x800539E0;
    constexpr uint32_t boot_rom = 0x1000, boot_vram = 0x80000400;

    bool inflate_main(std::vector<uint8_t>& main) {
        auto rom = recomp::get_rom();
        return rom.size() > main_rom && rush2::assets::inflate_raw(rom.data() + main_rom, rom.size() - main_rom, main);
    }
}

bool rush2::assets::boot_image(uint8_t* rdram) {
    auto rom = recomp::get_rom();
    std::vector<uint8_t> main;
    if (rom.size() < boot_rom + (main_vram - boot_vram) || !inflate_main(main)) return false;
    for (uint32_t i = 0; i < main_vram - boot_vram; i++) MEM_B(0, (int32_t)(boot_vram + i)) = rom[boot_rom + i];
    for (size_t i = 0; i < main.size(); i++) MEM_B(0, (int32_t)(main_vram + i)) = main[i];
    return true;
}

uint32_t rush2::assets::rom_offset(int index) {
    // The asset table 0x800C185C is in the main segment. Inflated once.
    constexpr uint32_t table_vram = 0x800C185C;
    constexpr int table_entries = 0x71;
    static std::mutex main_mutex;
    static std::vector<uint8_t> table;
    if (index < 0 || index >= table_entries) return 0;
    std::lock_guard lock{ main_mutex };
    if (table.empty()) {
        std::vector<uint8_t> main;
        if (!inflate_main(main)) return 0;
        size_t at = table_vram - main_vram;
        if (main.size() < at + table_entries * 4) return 0;
        table.assign(main.begin() + at, main.begin() + at + table_entries * 4);
    }
    const uint8_t* p = &table[size_t(index) * 4];
    return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
}

bool rush2::assets::rom_original(int index, std::vector<uint8_t>& out) {
    return read_rom_asset(index, rom_offset(index), out);
}

bool rush2::assets::read_current(int index, std::vector<uint8_t>& out, std::vector<Span>& spans) {
    uint8_t* rdram = game_rdram.load();
    if (rdram == nullptr || index < 0 || index >= asset_slots) {
        return false;
    }
    {
        std::lock_guard lock{ assets_mutex };
        auto it = replaced.find(index);
        if (it != replaced.end()) {
            out = it->second.base;
            spans = it->second.spans;
            return true;
        }
    }
    spans.clear();
    if ((uint32_t)MEM_W(0, (int32_t)(asset_offsets + index * 4)) == 0) {
        return false;
    }
    return read_original(rdram, index, out);
}

bool rush2::assets::replacement(int index, std::vector<uint8_t>& data, std::vector<Span>& spans) {
    std::lock_guard lock{ assets_mutex };
    auto it = replaced.find(index);
    if (it == replaced.end() || !it->second.own) return false;
    data = it->second.base;
    spans = it->second.spans;
    return true;
}

bool rush2::assets::is_replaced(int index) {
    std::lock_guard lock{ assets_mutex };
    auto it = replaced.find(index);
    return it != replaced.end() && it->second.own;
}

void rush2::assets::set_patcher(uint8_t* rdram, Patcher p) {
    std::lock_guard lock{ assets_mutex };
    patcher = std::move(p);
    for (int i = 0; i < asset_slots; i++) {
        auto it = replaced.find(i);
        if (it != replaced.end()) {
            Replacement& r = it->second;
            bool changed = patch(rdram, i, r);
            if (!r.own && r.data == r.base) { // only patched, and not any more: Rush 2's own again
                uninstall(rdram, i, r);
                replaced.erase(it);
                changes++;
            }
            else if (changed) {
                install(rdram, i, r);
                changes++;
            }
            continue;
        }
        if (!patcher) continue;
        uint32_t rom = (uint32_t)MEM_W(0, (int32_t)(asset_offsets + i * 4));
        if (rom == 0 || rom >= fake_rom_base) continue;
        Replacement r;
        r.rom = rom;
        r.size = (uint32_t)MEM_W(0, (int32_t)(asset_sizes + i * 4));
        r.own = false;
        if (!read_rom_asset(i, rom, r.base) || !patch(rdram, i, r)) continue;
        install(rdram, i, replaced.emplace(i, std::move(r)).first->second);
        changes++;
    }
}

// Start of func_80077F20 / func_80077F84: $a0 = ROM offset, $a1 = destination. Returns true (with $v0 = size) if
// the asset was served from a replacement. Either way the destination's range is tagged with the game its data
// comes from (rush2::origin).
extern "C" int rush2_asset_decompress(uint8_t* rdram, recomp_context* ctx) {
    uint32_t rom = (uint32_t)ctx->r4;
    int32_t dest = (int32_t)ctx->r5;
    game_rdram = rdram;
    std::lock_guard lock{ assets_mutex };
    auto fake = rom >= fake_rom_base ? by_fake_rom.find(rom) : by_fake_rom.end();
    if (fake == by_fake_rom.end()) {
        // Rush 2's own asset: its size from the size table, found by its ROM offset.
        for (int i = 0; i < asset_slots; i++) {
            if ((uint32_t)MEM_W(0, (int32_t)(asset_offsets + i * 4)) == rom) {
                rush2::origin::tag((uint32_t)dest, (uint32_t)MEM_W(0, (int32_t)(asset_sizes + i * 4)), rush2::origin::Game::Rush2);
                break;
            }
        }
        return 0;
    }
    const Replacement& r = replaced[fake->second];
    const std::vector<uint8_t>& data = r.data;
    for (size_t i = 0; i < data.size(); i++) {
        MEM_B(0, dest + (int32_t)i) = data[i];
    }
    rush2::origin::tag((uint32_t)dest, (uint32_t)data.size(), rush2::origin::Game::Rush2);
    for (const rush2::assets::Span& s : r.spans) {
        rush2::origin::tag((uint32_t)dest + s.offset, s.size, s.game);
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
    // The per-car-type tables move to copies with room for every type at the same time (include/car_types.h).
    rush2::car2049::init_tables(rdram);
    rush2::car2049::init_assets(rdram);
    rush2::fix_car_models(rdram);
    rush2::car_mods::init(rdram);
    rush2::paint_colors::init(rdram);
    rush2::car2049::init_physics(rdram);
    rush2::calspeed::init_cars(rdram);
}
