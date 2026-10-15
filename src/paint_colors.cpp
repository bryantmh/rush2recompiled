// Car paint colors with Rush 2049's shades (docs/rush2049_research/cars.md "Paint colors").
//
// Rush 2 has 32 paint colors, RGBA words at 0x800CE19C, and the MAIN, ACCENT and STRIPE COLOR rows step through them.
// Rush 2049's RGB table (0x801226C0) holds the same 32 colors index for index, retuned. While the 2049 cars are offered
// (Games tab), the 2049 shades that no Rush 2 color is close to (CIE76 dE of 10 or more from every one) are colors
// 32-52, for every car. Each has a stand-in: the Rush 2 color closest to it in hue, which a car shows when the extras
// aren't offered and which the Controller Pak record holds.
//
// The tables. The game reads the RGBA table in race_load_car_assets (0x80087008: the 2-player arrow), the car select
// preview (menu_car_select_run 0x803AF64C, 0x803AF6A0) and the swatch widget (0x803BABEC). race_scene_load_assets
// (0x8008A244) converts it to RGBA5551 words at 0x8010C180, which car_build_textures (0x80085C48: MAIN, ACCENT and
// STRIPE COLOR) and the HUD's map dots (hud_race_setup_widgets 0x800A0810, 0x800A0880) read by index. Both move to
// max_colors-entry copies in the recomp heap: a hook after each address load puts the copy in the register.
//
// The record. A player record's block per car type (record + 0x584 + type * 13) keeps the colors in 5 bits each: MAIN
// bits 5-7 of +1 and 0-1 of +2, ACCENT bits 2-6 of +2, STRIPE COLOR bits 3-7 of +3. The setters (save_set_car_logo_*:
// MAIN 0x80097A5C, ACCENT 0x800979C0, STRIPE COLOR 0x800978AC) are hooked at their inner call, where $a0 is the
// block's +1 and $a2 the value: an extra is stored as its stand-in, and the shade (by its 2049 index) goes in the
// saves' "paint" section keyed by that address. The getters (MAIN 0x800B2694, ACCENT 0x800B2640, STRIPE COLOR
// 0x800B25D0, read by carselect_build_list) give the extra back while it is offered and the record still holds its
// stand-in. A 2049 type's block is in its side slot (src/rush2049/car2049.cpp); the key is the record address it
// stands for, so the key doesn't depend on the slot.
//
// The car select (menu_screen_car_select_frame 0x803B9478) adds left/right to the row's byte (0x80201190 MAIN,
// 0x80201248 ACCENT, 0x802013B8 STRIPE COLOR; + (slot + 1) * 36 + type) and wraps it at 32. A hook before the add
// keeps the value, and one at the setter call (the row byte at $v0 + 0x24, value $v1) wraps it at count() instead. A
// Dreamcast 2049 car's MAIN row is its STYLE (src/rush2049/wings_menu.cpp) and is left alone.

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <mutex>
#include <string>
#include <utility>

#include "json/json.hpp"
#include "recomp.h"
#include "librecomp/addresses.hpp"

#include "car2049.h"
#include "data_files.h"
#include "paint_colors.h"
#include "rush2_hooks.h"

using namespace rush2::paint_colors;

namespace {
    // The extras in order (colors 32-52): Rush 2049's index of the shade and its Rush 2 stand-in.
    struct Extra {
        int index49;
        int stand_in;
    };
    constexpr Extra extras[extra_colors] = {
        { 2, 2 },   // gray 5A5A5A (dark gray)
        { 4, 5 },   // rose red DD4156 (red)
        { 9, 1 },   // ice blue C4D6DF (silver)
        { 10, 9 },  // sky blue 6FA0E3 (light blue)
        { 11, 10 }, // royal blue 1B38E5 (blue)
        { 12, 11 }, // dark blue 081B70 (navy)
        { 13, 12 }, // midnight blue 040E38 (dark navy)
        { 14, 16 }, // leaf green 58BB5B (green)
        { 15, 16 }, // emerald 00A92F (green)
        { 17, 17 }, // forest green 005A2A (dark green)
        { 18, 18 }, // aqua 3EC1BF (aqua)
        { 19, 20 }, // sea green 296855 (dark teal)
        { 20, 20 }, // dark teal 1B5653 (dark teal)
        { 21, 21 }, // orchid E525D0 (magenta)
        { 22, 22 }, // violet B74CE5 (lavender)
        { 24, 24 }, // purple 660FA9 (deep purple)
        { 26, 26 }, // gold yellow F0D41D (yellow)
        { 28, 28 }, // amber E08E1A (orange)
        { 29, 29 }, // khaki 8A6449 (brown)
        { 30, 1 },  // lilac gray C8B4CA (silver)
        { 31, 31 }, // taupe 504136 (dark brown)
    };

    constexpr uint32_t rush2_table = 0x800CE19C;     // Rush 2's RGBA table
    constexpr uint32_t row_value = 0x24;             // the car select row byte, from $v0
    constexpr uint32_t block_span = 0x584 + 36 * 13; // a record's blocks, its 2049 types' included
    constexpr int fields = 3;                        // MAIN, ACCENT, STRIPE COLOR

    std::array<std::atomic<uint32_t>, max_colors> colors = {};
    std::atomic_bool shades_loaded = false;
    uint32_t rgba_table = 0;     // RDRAM: RGBA words
    uint32_t rgba5551_table = 0; // RDRAM: RGBA5551 words (the game's 0x8010C180)

    uint32_t to_5551(uint32_t rgba) {
        uint32_t r = rgba >> 24, g = (rgba >> 16) & 0xFF, b = (rgba >> 8) & 0xFF;
        return ((r << 8) & 0xF800) | ((g << 3) & 0x7C0) | ((b >> 2) & 0x3E) | 1;
    }

    void write_tables(uint8_t* rdram) {
        if (rgba_table == 0) {
            return;
        }
        for (int i = 0; i < max_colors; i++) {
            uint32_t c = colors[i].load();
            MEM_W(0, (int32_t)(rgba_table + i * 4)) = (int32_t)c;
            MEM_W(0, (int32_t)(rgba5551_table + i * 4)) = (int32_t)to_5551(c);
        }
    }

    // The 2049 shades chosen for player records' fields: (record address of the block's +1, field) -> 2049 index.
    // Kept in the saves' "paint" section (include/data_files.h).
    const std::string section = "paint";
    std::mutex store_mutex;
    std::map<std::pair<uint32_t, int>, int> chosen;
    bool store_loaded = false;

    void load_store() {
        if (store_loaded) {
            return;
        }
        store_loaded = true;
        nlohmann::json j = nlohmann::json::parse(
            rush2::data_files::read(rush2::data_files::File::Saves, section), nullptr, false);
        if (!j.is_object() || !j.contains("colors") || !j["colors"].is_object()) {
            return;
        }
        for (auto& [key, value] : j["colors"].items()) {
            size_t colon = key.find(':');
            if (colon == std::string::npos || !value.is_number_integer()) {
                continue;
            }
            uint32_t at = (uint32_t)std::strtoul(key.substr(0, colon).c_str(), nullptr, 16);
            int field = std::atoi(key.c_str() + colon + 1);
            if (field >= 0 && field < fields) {
                chosen[{ at, field }] = value.get<int>();
            }
        }
    }

    void save_store() {
        nlohmann::json out = nlohmann::json::object();
        for (auto& [key, index49] : chosen) {
            char name[24];
            snprintf(name, sizeof(name), "%08X:%d", key.first, key.second);
            out[name] = index49;
        }
        rush2::data_files::write(rush2::data_files::File::Saves, section,
            nlohmann::json{ { "version", 1 }, { "colors", out } }.dump());
    }

    int extra_of(int index49) {
        for (int e = 0; e < extra_colors; e++) {
            if (extras[e].index49 == index49) {
                return e;
            }
        }
        return -1;
    }

    // The car select step in progress: the row's value before the game's change, or -1.
    int step_before = -1;
}

void rush2::paint_colors::init(uint8_t* rdram) {
    for (int i = 0; i < rush2_colors; i++) {
        colors[i] = (uint32_t)MEM_W(0, (int32_t)(rush2_table + i * 4));
    }
    for (int e = 0; e < extra_colors; e++) {
        colors[rush2_colors + e] = colors[extras[e].stand_in].load();
    }
    rgba_table = (uint32_t)((uint8_t*)recomp::alloc(rdram, max_colors * 4) - rdram) + 0x80000000u;
    rgba5551_table = (uint32_t)((uint8_t*)recomp::alloc(rdram, max_colors * 4) - rdram) + 0x80000000u;
    write_tables(rdram);
}

void rush2::paint_colors::load_2049(uint8_t* rdram, const uint8_t* table) {
    // A source without the table (a Dreamcast disc's Main segment is rebuilt from a list of tables) reads as zeros:
    // every one of 2049's colors is opaque.
    bool valid = table != nullptr;
    for (int i = 0; valid && i < rush2_colors; i++) {
        valid = table[i * 4 + 3] == 0xFF;
    }
    for (int e = 0; e < extra_colors; e++) {
        uint32_t c = colors[extras[e].stand_in].load();
        if (valid) {
            const uint8_t* p = table + extras[e].index49 * 4;
            c = (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | 0xFF;
        }
        colors[rush2_colors + e] = c;
    }
    shades_loaded = valid;
    write_tables(rdram);
}

int rush2::paint_colors::count() {
    return shades_loaded.load() && rush2::car2049::available() ? max_colors : rush2_colors;
}

int rush2::paint_colors::usable(int color) {
    if (color < 0 || color >= max_colors) {
        return 0;
    }
    if (color < rush2_colors || count() == max_colors) {
        return color;
    }
    return extras[color - rush2_colors].stand_in;
}

uint32_t rush2::paint_colors::rgba(int color) {
    return colors[std::clamp(color, 0, max_colors - 1)].load();
}

void rush2::paint_colors::forget_record(uint8_t* rdram, uint32_t record) {
    std::lock_guard lock{ store_mutex };
    load_store();
    if (std::erase_if(chosen, [record](const auto& e) {
            return e.first.first >= record && e.first.first < record + block_span;
        }) != 0) {
        save_store();
    }
}

// After an address load of Rush 2's RGBA table (0x800CE19C): the register gets the copy.
extern "C" void rush2_paint_rgba_table(uint8_t* rdram, recomp_context* ctx, uint64_t* reg) {
    if (rgba_table != 0) {
        *reg = (uint64_t)(int64_t)(int32_t)rgba_table;
    }
}

// After an address load of the RGBA5551 table (0x8010C180): the register gets the copy.
extern "C" void rush2_paint_5551_table(uint8_t* rdram, recomp_context* ctx, uint64_t* reg) {
    if (rgba5551_table != 0) {
        *reg = (uint64_t)(int64_t)(int32_t)rgba5551_table;
    }
}

// race_scene_load_assets at 0x8008A250, before its conversion loop: $v0 = the RGBA table, $v1 = the RGBA5551 table, $a0
// = the end of the RGBA table.
extern "C" void rush2_paint_convert(uint8_t* rdram, recomp_context* ctx) {
    if (rgba_table == 0) {
        return;
    }
    ctx->r2 = (uint64_t)(int64_t)(int32_t)rgba_table;
    ctx->r3 = (uint64_t)(int64_t)(int32_t)rgba5551_table;
    ctx->r4 = (uint64_t)(int64_t)(int32_t)(rgba_table + max_colors * 4);
}

// A record getter's value (*value, 0-31) of field `field` (0 MAIN, 1 ACCENT, 2 STRIPE COLOR) for the block whose +1 is
// at `at`: the 2049 shade chosen for it while the shades are offered and the record still holds its stand-in.
extern "C" void rush2_paint_get(uint8_t* rdram, recomp_context* ctx, uint32_t at, int field, uint64_t* value) {
    if (count() != max_colors) {
        return;
    }
    uint32_t key = rush2::car2049::record_address(rdram, at);
    std::lock_guard lock{ store_mutex };
    load_store();
    auto it = chosen.find({ key, field });
    if (it == chosen.end()) {
        return;
    }
    int e = extra_of(it->second);
    if (e >= 0 && extras[e].stand_in == (int)(*value & 0x1F)) {
        *value = (uint64_t)(rush2_colors + e);
    }
}

// A record setter at its inner call: $a0 = the block's +1, $a2 = the value of field `field`. A 2049 shade is kept
// beside the record, which gets its stand-in.
extern "C" void rush2_paint_set(uint8_t* rdram, recomp_context* ctx, int field) {
    uint32_t at = (uint32_t)ctx->r4;
    int value = (int)(ctx->r6 & 0xFF);
    uint32_t key = rush2::car2049::record_address(rdram, at);
    std::lock_guard lock{ store_mutex };
    load_store();
    bool changed;
    if (value >= rush2_colors && value < max_colors) {
        const Extra& extra = extras[value - rush2_colors];
        auto it = chosen.find({ key, field });
        changed = it == chosen.end() || it->second != extra.index49;
        chosen[{ key, field }] = extra.index49;
        ctx->r6 = (uint64_t)extra.stand_in;
    }
    else {
        changed = chosen.erase({ key, field }) != 0;
    }
    if (changed) {
        save_store();
    }
}

// Car select, before the game adds the step to a color row's byte: `value` is the byte.
extern "C" void rush2_paint_step_before(uint8_t* rdram, recomp_context* ctx, int value) {
    step_before = value;
}

// Car select, at the setter call after the change: $v0 + 0x24 = the row byte, $v1 = its value, $a3 = the type, $a1 =
// the step. With the 2049 shades the row wraps at count().
extern "C" void rush2_paint_step_value(uint8_t* rdram, recomp_context* ctx, int field) {
    int before = step_before;
    step_before = -1;
    int step = (int32_t)ctx->r5 > 0 ? 1 : (int32_t)ctx->r5 < 0 ? -1 : 0;
    int type = (int)(int8_t)ctx->r7;
    int n = count();
    if (before < 0 || step == 0 || (before < rush2_colors && n == rush2_colors)) {
        return;
    }
    if (field == 0 && rush2::car2049::paint_jobs(type) > 0) {
        return;
    }
    int value = ((std::min(before, n - 1) + step) % n + n) % n;
    MEM_B(0, (int32_t)((uint32_t)ctx->r2 + row_value)) = (int8_t)value;
    ctx->r3 = (uint64_t)value;
}
