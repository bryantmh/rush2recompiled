// Rush 1's car decals as a ninth STRIPE value, "SF RUSH", on the Rush 2 cars that have one of their own (the Camaro, VW
// Bus, VW Bug, Taxi and Bugatti). docs/rush1_research.md section 11; the masks come from src/rush1/car1_decals.cpp.
//
// Rush 2 paints a car's stripe in func_8008582C: for each of the car's 24 panel textures (D0 and D1 damage stages x 6
// panels x full and quarter-size mip) func_800843EC looks up the stripe's tile texture by name (prefix table
// 0x800C6518 indexed by the STRIPE value, then the panel's name) and func_80083F50 stamps it onto the panel in the
// STRIPE COLOR. A tile is a 64x32 / 32x64 texture whose bytes are 0xF0 | alpha (0xFF stripe, 0xF0 none).
//
// The value 8 is stored the way Rush 2 stores the others, as the row's byte in the car select's per-type tables
// (0x80201300 + ..., 36 wide), but the player record only has 3 bits for it: the record block's byte 0x585 (MAIN's
// low bits are above 0x1F, so bits 0-4 are free and every setter keeps them) holds an "SF Rush" flag, and the STRIPE
// field is 0 while it is set. The getter and setter are hooked to convert between the two.
//
// To paint it, the lookup runs as for value 1 (SINGLE), and in place of the stamp the decal's texels are written
// into the panel texture in Rush 1's own colours (fixed car palette entries, so STRIPE COLOR doesn't apply). The rest
// of the panel keeps Rush 2's paint, so MAIN and ACCENT still colour the car.

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif

#include "recomp.h"
#include "librecomp/addresses.hpp"
#include "librecomp/game.hpp"
#include "util/file.h"

#include "assets.h"
#include "car1_decals.h"
#include "car1_stripes.h"
#include "rush2_hooks.h"
#include "track1.h"

extern "C" void pak_mark_dirty_8005F338(uint8_t* rdram, recomp_context* ctx);   // Marks player-record bytes dirty for the pak save.

namespace {
    constexpr int sf_rush_value = 8;                  // the STRIPE value (Rush 2's are 0-7)
    constexpr uint32_t bank_table = 0x80119220;       // [bank] = texture records, count (record: name[16], u16 w, h, ..., +0x18 texels)
    constexpr int stripe_asset = 0x1C;                // Rush 2 asset with the stripe tiles and CARPALETTE
    constexpr int first_car_asset = 0x1D;
    constexpr uint32_t record_flag = 0x585;           // block byte, bit 0: the SF Rush stripe is chosen
    constexpr int block_size = 13;

    std::atomic_bool option_enabled = true;

    struct CarState {
        bool built = false, ok = false;
        rush2::car1decals::Pattern pattern;
    };
    CarState states[rush2::car1decals::car_count];
    uint32_t text_address = 0;

    int car_of(int type) {
        for (int i = 0; i < rush2::car1decals::car_count; i++) {
            if (rush2::car1decals::cars[i].rush2_type == type) {
                return i;
            }
        }
        return -1;
    }

    // The built decals are kept on disk, one file per car in <app folder>/stripe_cache, so a car's decal is built once
    // rather than in every session. A file is used only if it holds the same key: the hash of the three input files and
    // of the executable's size and modification time, so another ROM or a new build (a changed builder) builds again.
    // File: "R2SC", u32 version, u64 key, then per panel 1-6: i32 w, h, u32 full size, bytes, u32 lod size, bytes.
    constexpr char cache_magic[4] = { 'R', '2', 'S', 'C' };
    constexpr uint32_t cache_version = 1;

    uint64_t hash(const uint8_t* data, size_t size, uint64_t h = 0xCBF29CE484222325ull) {
        for (size_t i = 0; i < size; i++) {
            h = (h ^ data[i]) * 0x100000001B3ull;
        }
        return h;
    }

    uint64_t build_stamp() {
        static uint64_t stamp = [] {
            std::filesystem::path exe;
#ifdef _WIN32
            wchar_t buf[MAX_PATH * 4];
            DWORD n = GetModuleFileNameW(nullptr, buf, (DWORD)std::size(buf));
            if (n > 0 && n < std::size(buf)) {
                exe = std::filesystem::path(std::wstring(buf, n));
            }
#else
            std::error_code ec0;
            exe = std::filesystem::read_symlink("/proc/self/exe", ec0);
#endif
            std::error_code ec;
            uint64_t v[2] = { exe.empty() ? 0 : (uint64_t)std::filesystem::file_size(exe, ec),
                              exe.empty() ? 0 : (uint64_t)std::filesystem::last_write_time(exe, ec).time_since_epoch().count() };
            return hash((const uint8_t*)v, sizeof(v));
        }();
        return stamp;
    }

    std::filesystem::path cache_path(const char* car) {
        return recomp::get_config_path() / "stripe_cache" / (std::string(car) + ".bin");
    }

    bool cache_load(const char* car, uint64_t key, rush2::car1decals::Pattern& out) {
        std::ifstream f(cache_path(car), std::ios::binary);
        if (!f) {
            return false;
        }
        std::vector<uint8_t> b((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        size_t at = 0;
        auto take = [&](void* dst, size_t n) {
            if (n > b.size() - at) {
                return false;
            }
            memcpy(dst, b.data() + at, n);
            at += n;
            return true;
        };
        char magic[4];
        uint32_t version;
        uint64_t stored;
        if (!take(magic, 4) || memcmp(magic, cache_magic, 4) != 0 || !take(&version, 4) || version != cache_version ||
            !take(&stored, 8) || stored != key) {
            return false;
        }
        rush2::car1decals::Pattern p;
        for (int n = 1; n <= 6; n++) {
            rush2::car1decals::Panel& panel = p.panel[n];
            uint32_t size;
            if (!take(&panel.w, 4) || !take(&panel.h, 4) || panel.w < 0 || panel.h < 0 || panel.w > 256 || panel.h > 256) {
                return false;
            }
            for (std::vector<uint8_t>* v : { &panel.full, &panel.lod }) {
                if (!take(&size, 4) || size > b.size() - at) {
                    return false;
                }
                v->assign(b.begin() + at, b.begin() + at + size);
                at += size;
            }
        }
        out = std::move(p);
        return true;
    }

    void cache_save(const char* car, uint64_t key, const rush2::car1decals::Pattern& p) {
        std::vector<uint8_t> b;
        auto put = [&](const void* src, size_t n) {
            b.insert(b.end(), (const uint8_t*)src, (const uint8_t*)src + n);
        };
        put(cache_magic, 4);
        put(&cache_version, 4);
        put(&key, 8);
        for (int n = 1; n <= 6; n++) {
            const rush2::car1decals::Panel& panel = p.panel[n];
            put(&panel.w, 4);
            put(&panel.h, 4);
            for (const std::vector<uint8_t>* v : { &panel.full, &panel.lod }) {
                uint32_t size = (uint32_t)v->size();
                put(&size, 4);
                put(v->data(), v->size());
            }
        }
        std::filesystem::path path = cache_path(car);
        std::error_code ec;
        std::filesystem::create_directories(path.parent_path(), ec);
        std::filesystem::path tmp = path;
        tmp += ".tmp";
        {
            std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
            if (!f.write((const char*)b.data(), (std::streamsize)b.size())) {
                return;
            }
        }
        std::filesystem::rename(tmp, path, ec);
    }

    // Builds car c's decal, or loads it from the cache (building takes a few tens of milliseconds per car).
    bool ensure(uint8_t* rdram, int c) {
        CarState& s = states[c];
        if (s.built) {
            return s.ok;
        }
        auto rom1 = rush2::track1::get_rom();
        if (rom1 == nullptr) {
            return false;
        }
        s.built = true;
        const rush2::car1decals::Car& car = rush2::car1decals::cars[c];
        std::vector<uint8_t> r1, r2, stripes;
        const char* failed = nullptr;
        const char* why = "";
        if (!rush2::track1::read_asset(*rom1, car.rush1_asset, r1)) {
            failed = "reading the Rush 1 car file";
        }
        else if (!rush2::assets::read_original(rdram, first_car_asset + car.rush2_type, r2)) {
            failed = "reading the Rush 2 car file";
        }
        else if (!rush2::assets::read_original(rdram, stripe_asset, stripes)) {
            failed = "reading Rush 2's stripe tiles";
        }
        else {
            uint64_t key = hash(r1.data(), r1.size(), build_stamp());
            key = hash(r2.data(), r2.size(), key);
            key = hash(stripes.data(), stripes.size(), key);
            if (!cache_load(car.name, key, s.pattern)) {
                if (rush2::car1decals::build(c, r1, r2, stripes, s.pattern, &why)) {
                    cache_save(car.name, key, s.pattern);
                }
                else {
                    failed = "building the masks";
                }
            }
        }
        if (failed != nullptr) {
            printf("[Rush1] Couldn't build the %s stripe: %s failed (%s; files %zu, %zu, %zu bytes)\n", car.name, failed, why,
                   r1.size(), r2.size(), stripes.size());
            fflush(stdout);
            return false;
        }
        // RUSH2_CAR1_DUMP=<dir>: writes each panel's decal texels (<car>_<n>.bin, w*h bytes) to compare with
        // tools/rush1/cardecal.py.
        const char* dump = std::getenv("RUSH2_CAR1_DUMP");
        for (int n = 1; n <= 6; n++) {
            const rush2::car1decals::Panel& p = s.pattern.panel[n];
            s.ok |= std::any_of(p.full.begin(), p.full.end(), [](uint8_t c) { return c != 0; });
            if (dump != nullptr && !p.full.empty()) {
                std::string path = std::string(dump) + "/" + car.name + "_" + std::to_string(n) + ".bin";
                if (FILE* f = std::fopen(path.c_str(), "wb")) {
                    std::fwrite(p.full.data(), 1, p.full.size(), f);
                    std::fclose(f);
                }
            }
        }
        return s.ok;
    }

    // Whether car type `type` offers the SF Rush stripe now.
    bool eligible(uint8_t* rdram, int type) {
        int c = car_of(type);
        return c >= 0 && option_enabled.load(std::memory_order_relaxed) && ensure(rdram, c);
    }

    // Car select, the STRIPE row: the value before the game's change and the new one.
    int stripe_before = -1;
    bool stripe_text_row = false;

    bool paint_sf_rush = false;     // func_8008582C's loop is on the SF Rush value
}

void rush2::car1stripes::set_option(bool enabled) {
    option_enabled = enabled;
}

// Player record getter func_800B2608 at 0x800B2624 ($t8 = the car's block, $a1 = type): the STRIPE value, 8 when the
// block's SF Rush flag is set. Returns true (the function returns $v0).
extern "C" int rush2_car1_stripe_get(uint8_t* rdram, recomp_context* ctx) {
    uint32_t block = (uint32_t)ctx->r24;
    int type = (int)(ctx->r5 & 0xFF);
    int value = MEM_BU(0, (int32_t)(block + 0x587)) & 7;
    if ((MEM_BU(0, (int32_t)(block + record_flag)) & 1) && eligible(rdram, type)) {
        value = sf_rush_value;
    }
    ctx->r2 = (uint64_t)value;
    return 1;
}

// Player record setter func_80097934 at 0x80097960 ($a0 = the block's byte 0x585, $a1 = type, $a2 = value, before
// the stripe field is written): the SF Rush flag is set with the value 8 and cleared by any other, and the STRIPE field
// is 0 for 8.
extern "C" void rush2_car1_stripe_set(uint8_t* rdram, recomp_context* ctx) {
    uint32_t at = (uint32_t)ctx->r4;
    int type = (int)(ctx->r5 & 0xFF);
    int value = (int)(ctx->r6 & 0xFF);
    uint8_t before = MEM_BU(0, (int32_t)at);
    uint8_t now = before;
    if (value == sf_rush_value) {
        now |= 1;
        ctx->r6 = 0;
    }
    else if (eligible(rdram, type)) {
        now &= ~1;
    }
    if (now != before) {
        MEM_B(0, (int32_t)at) = (int8_t)now;
        recomp_context call = *ctx;
        call.r4 = (uint64_t)(int64_t)(int32_t)at;
        call.r5 = 1;
        pak_mark_dirty_8005F338(rdram, &call);
    }
}

// Car select func_803B9478, the STRIPE row. At 0x803B9B18 $t9 is the row's value before the game's change.
extern "C" void rush2_car1_stripe_before(uint8_t* rdram, recomp_context* ctx) {
    stripe_before = (int8_t)ctx->r25;
}

// At 0x803B9B8C the game has changed the value ($v1, stored at $v0 + 0x24; type $a3, direction $a1): a car with the SF
// Rush stripe cycles through 9 values.
extern "C" void rush2_car1_stripe_value(uint8_t* rdram, recomp_context* ctx) {
    int before = stripe_before;
    stripe_before = -1;
    int type = (int)(int8_t)ctx->r7;
    int step = (int32_t)ctx->r5 > 0 ? 1 : (int32_t)ctx->r5 < 0 ? -1 : 0;
    if (before < 0 || step == 0 || !eligible(rdram, type)) {
        return;
    }
    int value = (std::clamp(before, 0, sf_rush_value) + step + sf_rush_value + 1) % (sf_rush_value + 1);
    MEM_B(0, (int32_t)((uint32_t)ctx->r2 + 0x24)) = (int8_t)value;
    ctx->r3 = (uint64_t)value;
}

// Car select func_803BC048: the STRIPE row's case starts at 0x803BC3F4, and its value text is in $s0 at 0x803BC644 ($t3 =
// the value). Value 8 reads past the row's 8 strings, so it gets its own.
extern "C" void rush2_car1_stripe_text_row(uint8_t* rdram, recomp_context* ctx) {
    stripe_text_row = true;
}

extern "C" void rush2_car1_stripe_text(uint8_t* rdram, recomp_context* ctx) {
    bool row = stripe_text_row;
    stripe_text_row = false;
    if (!row || (int8_t)ctx->r11 != sf_rush_value) {
        return;
    }
    if (text_address == 0) {
        text_address = (uint32_t)((uint8_t*)recomp::alloc(rdram, 16) - rdram) + 0x80000000u;
        for (int i = 0; i < 8; i++) {
            MEM_B(0, (int32_t)(text_address + i)) = "SF RUSH"[i];
        }
    }
    ctx->r16 = (uint64_t)(int64_t)(int32_t)text_address;
}

// func_8008582C's stripe loop. At 0x80085D84, before the tile lookup func_800843EC ($t0 = the STRIPE value): the
// SF Rush value looks up SINGLE's tile so that the loop goes on to the stamp.
extern "C" void rush2_car1_paint_style(uint8_t* rdram, recomp_context* ctx) {
    paint_sf_rush = (int8_t)ctx->r8 == sf_rush_value;
    if (paint_sf_rush) {
        ctx->r8 = 1;
    }
}

// At 0x80085D8C, after the lookup ($v0 = the tile's record or 0; the panel texture's handle at sp + 0xBE, the car's
// type at sp + 0xCE): the decal is written into the panel's texels and the stamp is skipped. As for the other stripes,
// the loop first copies the panel's clean texels back (func_80007610) when the game keeps a copy.
extern "C" void rush2_car1_paint_stamp(uint8_t* rdram, recomp_context* ctx) {
    if (!paint_sf_rush) {
        return;
    }
    paint_sf_rush = false;
    ctx->r2 = 0;    // skips the stamp
    uint32_t sp = (uint32_t)ctx->r29;
    int type = (int16_t)MEM_H(0, (int32_t)(sp + 0xCE));
    int c = car_of(type);
    if (c < 0 || !ensure(rdram, c)) {
        return;
    }
    // The panel texture's record: the name ends _D<stage>_<n> or _D<stage>_<n>_4 (the quarter-size mip). The D1
    // (damaged) panels share the D0 layout.
    uint32_t handle = MEM_HU(0, (int32_t)(sp + 0xBE));
    uint32_t records = (uint32_t)MEM_W(0, (int32_t)(bank_table + (handle >> 10) * 8));
    uint32_t record = records + (handle & 0x3FF) * 0x20;
    char name[17] = {};
    for (int i = 0; i < 16; i++) {
        name[i] = (char)MEM_BU(0, (int32_t)(record + i));
    }
    // Panel 4's full-size texture also ends in _4 (_D0_4), so the mip is told apart by the name's length.
    std::string s = name;
    size_t at = s.rfind("_D", s.size() >= 7 ? s.size() - 7 : 0);
    bool lod = at != std::string::npos && s.size() == at + 7 && s.compare(at + 5, 2, "_4") == 0;
    if (!lod) {
        at = s.rfind("_D");
    }
    int n = (at != std::string::npos && (s.size() == at + 5 || lod) && s[at + 3] == '_') ? s[at + 4] - '0' : 0;
    int w = (int)MEM_HU(0, (int32_t)(record + 0x10)), h = (int)MEM_HU(0, (int32_t)(record + 0x12));
    const std::vector<uint8_t>* texels = nullptr;
    if (n >= 1 && n <= 6) {
        const rush2::car1decals::Panel& p = states[c].pattern.panel[n];
        if (p.w != 0 && w == (lod ? p.w / 4 : p.w) && h == (lod ? p.h / 4 : p.h)) {
            texels = lod ? &p.lod : &p.full;
        }
    }
    if (std::getenv("RUSH2_CAR1_DUMP") != nullptr) {
        printf("[Rush1] stamp %s %dx%d data %08X decal %s\n", name, w, h, (uint32_t)MEM_W(0, (int32_t)(record + 0x18)),
               texels != nullptr ? "yes" : "no");
        fflush(stdout);
    }
    if (texels == nullptr) {
        return;
    }
    // RDRAM is word-swapped on the host: bytes go through MEM_B.
    uint32_t data = (uint32_t)MEM_W(0, (int32_t)(record + 0x18));
    for (int i = 0; i < w * h; i++) {
        if ((*texels)[i] != 0) {
            MEM_B(0, (int32_t)(data + i)) = (int8_t)(*texels)[i];
        }
    }
}
