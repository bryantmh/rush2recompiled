// Rush 1's car decals as a ninth STRIPE value, "SF RUSH", on the Rush 2 cars that have one of their own (the Camaro, VW
// Bus, VW Bug and Taxi). docs/rush1_research.md section 11; the masks come from src/car1_decals.cpp.
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
// into the panel texture in Rush 1's own colours (fixed car palette entries, so STRIPE COLOR doesn't apply), and the
// panel's accent-colour areas take the main colour, as Rush 1 paints these cars in one colour.

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "recomp.h"
#include "librecomp/addresses.hpp"

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

    // Builds car c's decal (once: it takes a few tens of milliseconds).
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
        else if (!rush2::car1decals::build(c, r1, r2, stripes, s.pattern, &why)) {
            failed = "building the masks";
        }
        if (failed != nullptr) {
            printf("[Rush1] Couldn't build the %s stripe: %s failed (%s; files %zu, %zu, %zu bytes)\n", car.name, failed, why,
                   r1.size(), r2.size(), stripes.size());
            fflush(stdout);
            return false;
        }
        for (int n = 1; n <= 6; n++) {
            const rush2::car1decals::Panel& p = s.pattern.panel[n];
            s.ok |= std::any_of(p.full.begin(), p.full.end(), [](uint8_t c) { return c != 0; });
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
    std::string s = name;
    bool lod = s.size() > 2 && s.compare(s.size() - 2, 2, "_4") == 0;
    if (lod) {
        s.resize(s.size() - 2);
    }
    size_t at = s.rfind("_D");
    int n = (at != std::string::npos && s.size() == at + 5 && s[at + 3] == '_') ? s[at + 4] - '0' : 0;
    int w = (int)MEM_HU(0, (int32_t)(record + 0x10)), h = (int)MEM_HU(0, (int32_t)(record + 0x12));
    const std::vector<uint8_t>* texels = nullptr;
    if (n >= 1 && n <= 6) {
        const rush2::car1decals::Panel& p = states[c].pattern.panel[n];
        if (p.w != 0 && w == (lod ? p.w / 4 : p.w) && h == (lod ? p.h / 4 : p.h)) {
            texels = lod ? &p.lod : &p.full;
        }
    }
    // Rush 1 paints these cars in one colour: Rush 2's accent-colour areas (accent ramp 33-63) take the same shade of
    // the main ramp (1-31), then the decal goes on. RDRAM is word-swapped on the host: bytes go through MEM_B.
    uint32_t data = (uint32_t)MEM_W(0, (int32_t)(record + 0x18));
    for (int i = 0; i < w * h; i++) {
        uint8_t texel = texels != nullptr ? (*texels)[i] : 0;
        if (texel == 0) {
            texel = MEM_BU(0, (int32_t)(data + i));
            if (texel < 33 || texel > 63) {
                continue;
            }
            texel -= 32;
        }
        MEM_B(0, (int32_t)(data + i)) = (int8_t)texel;
    }
}
