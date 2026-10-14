// Fixes for bugs in the original Rush 2 (US), and small gameplay options. Each is a small hook into the game's own
// code; the fixes have no toggle unless noted.
//
// Mountain Dew dragster paint (GitHub issue #5). func_803B9478 (car select frame) works out every frame, at
// 0x803BA644-0x803BA7E4, whether slot $s7's cursor row can be changed for the slot's car type, into the byte
// 0x803CB3AC[slot] (left and right do nothing on the row while it is 0, 0x803B983C). By option id
// (0x803CB3B8[row]): the DEW (type 21) blocks 2 MAIN COLOR, 3 ACCENT COLOR, 4 STRIPE and 5 STRIPE COLOR; the TAXI
// (16) blocks 3; FORMULA (18) blocks 4 and 5; ROCKET (20) blocks 1, 4, 5, 8 and $s3's id. Yet the car paint builder
// (func_8008582C) still tints the DEW: its palette class table 0x800C5698 (the other cars use 0x800C5670) gives
// entries 1-31 MAIN (flags 0x22) and 33-63 ACCENT (0x42) like every car, and fixes 64-255, which is why computer
// opponents in a Dew show other colors. It only skips the stripe for types 18, 20 and 21 (0x80085CA8). So MAIN and
// ACCENT are unblocked for the DEW; STRIPE and STRIPE COLOR stay blocked, as the builder would ignore them.
//
// Engines silent after the clock runs out (GitHub issue #4). In a race (game state 3, func_800AE670 at 0x800AEC14),
// once the time left is down to 0.5 s the game sets the out of time flag 0x800FAE98 and the engines off byte
// 0x800E7BCE (0x800AEF94), which makes func_800650DC give the engine sounds rpm 0. A checkpoint reached while
// coasting adds time, and the next frame clears 0x800FAE98 (0x800AEDC4), but nothing clears 0x800E7BCE until the next
// race setup (func_800A5ADC, 0x800A6134), so the engines stay silent for the rest of the race. Rush 2049 fixed it;
// here it is cleared along with the flag.
//
// Finish place lost to DEATHS (user report). With DEATHS on (0x8010BC98), func_8009E180 (respawn) case 2 is a car
// out of deaths: it sets the car state's out byte +0x343 and calls race_car_finish (func_8008E8EC) at the race time,
// and race_car_finish ranks every car with +0x343 > 0 behind all the finishers. A car that had already finished
// (+0xEA) and then wrecked lost its place to the last one left. A finished car's case 2 becomes a plain respawn.
//
// White accent under a white body (user report). func_8008582C copies the car's base palette (CARPALETTE) and, for a
// white color (index 0), brightens its ramp first: MAIN COLOR's entries 1-31 when MAIN is white (0x80085898-),
// otherwise ACCENT COLOR's entries 33-63 when ACCENT is white (0x80085960-, an else-if). Each entry becomes a gray of
// its red channel x 8 x 1.25 (at most 255). With both white the accent ramp was never brightened, so the accent came
// out darker than with any other body color. The hook at 0x80085C14, where the branches join, brightens it then.
//
// Rocket windshield (user report). The ROCKET's (type 20, asset 0x31) container has no texture table: its 11 CI8
// textures load through the container's texture display lists (0x4830-0x4AF0). Its windshield is a solid block of
// palette entry 17 framed by entries 179-185 in texture 6 (32 x 64, every one of its 184 texels of entry 17), and
// 1-31 are MAIN COLOR (class table 0x800C5670), so the windshield took the MAIN COLOR. At boot the asset is replaced
// by a copy whose texture 6 uses entry 190 there (CARPALETTE 8, 8, 8), the near-black of the rest of the windshield.
//
// Formula suspension (user report). The FORMULA's (type 18, asset 0x2F) wishbones are one 64 x 32 CI8 texture (0x828,
// a V, wide at texel 0, apex at 63) on two strips across the car in FORM1D0_FR1 / D0_RR1, s from -62.6 at x = +38 to
// 63.5 at x = -38: meant to mirror at the middle, but its render tile wraps (SETTILE 0x00014060, also in SF Rush), so
// the right-hand wishbones ran backwards, apex at the chassis. The copy's tile mirrors in S. Every car's wheels sit at
// z = +-3.925 ft (descriptor +0x7C; model units are 1/16 ft, so +-62.8), which matches the front arms' hubs (z about
// 58) but leaves the rear wheels 22 units ahead of the rear arms' hubs (z about -85, under the wing) and inside the
// sidepods. The wheel draw (func_8005A598) moves the FORMULA's rear wheels back to the hubs; physics is unchanged.
//
// Stallion spoiler (user report). The STALLION's (type 14, asset 0x2B) body is symmetric about x = 0, but its spoiler,
// in every TOP model (D0_TOP1, D1_TOP1, D0_DTOP1, D0_D2TOP2; the only vertices there behind z = -80), runs from x = -36
// to 39 (trailing edge -13 to 17). The copy moves those vertices 2 units left (trailing edge -15 to 15, like the deck
// under it at +-16).
//
// Respawn gear (option, off by default). func_8009E180 puts player 1's car (car table 0x800D9C10 + car * 9,
// +8 driver == 8) in 2nd gear (car +0x738 = 2, 0x8009E3E0) on every respawn, which a manual gearbox then has to shift
// down from at slow restarts (stunt and battle arenas, practice). The wrecked car then glides to the respawn point
// (car_wreck_steer_to_respawn, func_800754B4, ends it with car +0x6C8 = -2), and func_8008E258 finishes the respawn
// of every such car with 2nd gear again (+0x738, +0x3E0 and +0x3DE = 2, 0x8008E330). On, every mode puts the human
// cars in 1st at both.

#include <algorithm>
#include <atomic>
#include <cstring>
#include <string>
#include <vector>

#include "recomp.h"
#include "assets.h"
#include "rush2.h"

#include "rush2_hooks.h"

namespace {
    constexpr uint32_t option_ids = 0x803CB3B8;     // s32 [16]: car select rows' option ids
    constexpr uint32_t slot_cursor = 0x803C6990;    // s32 [2]: each panel's cursor row
    constexpr uint32_t slot_type = 0x803CB362;      // s8 [2]: each panel's car type
    constexpr uint32_t row_changeable = 0x803CB3AC; // u8 [2]: the cursor row can be changed (left / right)
    constexpr int type_dew = 21;
    constexpr int option_main = 2;
    constexpr int option_accent = 3;

    constexpr uint32_t engines_off = 0x800E7BCE;    // u8: engine sounds get rpm 0 (out of time)

    constexpr uint32_t car_states = 0x801124A0;     // 0x354 each: +0xEA finished, +0x343 out of deaths
    constexpr int respawn_out_of_deaths = 2;

    constexpr uint32_t car_kind = 0x7E8;            // u8 in the physics car: 1 drone, 2 human
    constexpr int car_human = 2;
    std::atomic<bool> stunt_first_gear = false;
}

void rush2::set_stunt_first_gear(bool enabled) {
    stunt_first_gear.store(enabled, std::memory_order_relaxed);
}

// func_803B9478 at 0x803BA7E8, after slot $s7's row-changeable byte is set: MAIN and ACCENT COLOR for the Dew.
extern "C" void rush2_fix_dew_paint_rows(uint8_t* rdram, recomp_context* ctx) {
    int slot = (int32_t)ctx->r23;
    if (slot < 0 || slot > 1 || (int8_t)MEM_B(0, (int32_t)(slot_type + slot)) != type_dew) {
        return;
    }
    int row = MEM_W(0, (int32_t)(slot_cursor + slot * 4));
    if (row < 0 || row >= 16) {
        return;
    }
    int option = MEM_W(0, (int32_t)(option_ids + row * 4));
    if (option == option_main || option == option_accent) {
        MEM_B(0, (int32_t)(row_changeable + slot)) = 1;
    }
}

// func_800AE670 at 0x800AEDC0, about to clear the out of time flag (time was added): the engines run again.
extern "C" void rush2_fix_engines_after_timeout(uint8_t* rdram, recomp_context* ctx) {
    MEM_B(0, (int32_t)engines_off) = 0;
}

// func_8009E180 entry: $a0 = respawn case, $a1 = car. A finished car out of deaths respawns instead of being ranked out.
extern "C" void rush2_fix_finish_place(uint8_t* rdram, recomp_context* ctx) {
    int car = (int16_t)ctx->r5;
    if ((int16_t)ctx->r4 != respawn_out_of_deaths || car < 0 || car >= 8) {
        return;
    }
    if (MEM_B(0, (int32_t)(car_states + car * 0x354 + 0xEA)) == 1) {
        ctx->r4 = 1;
    }
}

// func_8009E180 at 0x8009E3E4, right after a respawn put player 1's car in 2nd gear: $a3 = car.
extern "C" void rush2_stunt_respawn_gear(uint8_t* rdram, recomp_context* ctx) {
    if (stunt_first_gear.load(std::memory_order_relaxed)) {
        MEM_H(0x738, ctx->r7) = 1;
    }
}

// func_8008E258 at 0x8008E340, after a car that finished its glide to the respawn point was put in 2nd gear
// (+0x738 selected, +0x3E0 and +0x3DE the gearbox's): $s0 = physics car.
extern "C" void rush2_stunt_respawn_gear_reset(uint8_t* rdram, recomp_context* ctx) {
    int32_t car = (int32_t)ctx->r16;
    if (stunt_first_gear.load(std::memory_order_relaxed) && MEM_BU(0, (int32_t)(car + car_kind)) == car_human) {
        MEM_H(0x738, car) = 1;
        MEM_H(0x3E0, car) = 1;
        MEM_H(0x3DE, car) = 1;
    }
}

// func_8008582C at 0x80085C14: 0xA4($sp) = the palette copy, 0xD3($sp) = MAIN COLOR, 0xD7($sp) = ACCENT COLOR.
extern "C" void rush2_fix_white_accent(uint8_t* rdram, recomp_context* ctx) {
    int32_t sp = (int32_t)ctx->r29;
    if (MEM_BU(0xD3, sp) != 0 || MEM_BU(0xD7, sp) != 0) {
        return;
    }
    int32_t palette = MEM_W(0xA4, sp);
    for (int i = 33; i < 64; i++) {
        uint16_t e = MEM_HU(i * 2, palette);
        int v = (int)((float)(((e >> 11) & 0x1F) * 8) * 1.25f);
        if (v >= 0x100) {
            v = 0xFF;
        }
        MEM_H(i * 2, palette) = (int16_t)(((v << 8) & 0xF800) | ((v * 8) & 0x7C0) | ((v >> 2) & 0x3E) | 1);
    }
}

namespace {
    uint32_t be32(const std::vector<uint8_t>& d, size_t o) {
        return (uint32_t)d[o] << 24 | (uint32_t)d[o + 1] << 16 | (uint32_t)d[o + 2] << 8 | d[o + 3];
    }

    void put32(std::vector<uint8_t>& d, size_t o, uint32_t v) {
        d[o] = (uint8_t)(v >> 24); d[o + 1] = (uint8_t)(v >> 16); d[o + 2] = (uint8_t)(v >> 8); d[o + 3] = (uint8_t)v;
    }

    // A car container's texture load lists (header words 7-8): calls fn(offset, w0, w1) for each command.
    template <typename Fn> void for_each_texture_load(const std::vector<uint8_t>& d, Fn fn) {
        for (uint32_t o = be32(d, 28); o + 8 <= be32(d, 32) && o + 8 <= d.size(); o += 8) {
            fn(o, be32(d, o), be32(d, o + 4));
        }
    }

    // The vertex offsets loaded (G_VTX, 0x01) by the display lists of every model whose name contains `part`.
    std::vector<uint32_t> model_vertices(const std::vector<uint8_t>& d, const char* part) {
        std::vector<uint32_t> out;
        uint32_t models = be32(d, 0), names = be32(d, 4), count = be32(d, 16);
        for (uint32_t m = 0; m < count && names + m * 0x18 + 16 <= d.size(); m++) {
            std::string name((const char*)&d[names + m * 0x18], strnlen((const char*)&d[names + m * 0x18], 16));
            if (name.find(part) == std::string::npos) {
                continue;
            }
            uint32_t rec = models + m * 0x34, lods = be32(d, rec);
            for (uint32_t l = 0; l < lods && l < 4; l++) {
                std::vector<uint32_t> lists = { be32(d, rec + 4 + l * 12 + 8) & 0xFFFFFF };
                while (!lists.empty()) {
                    uint32_t o = lists.back();
                    lists.pop_back();
                    for (; o + 8 <= d.size(); o += 8) {
                        uint32_t w0 = be32(d, o), w1 = be32(d, o + 4);
                        if (w0 >> 24 == 0x01) {
                            for (uint32_t k = 0; k < ((w0 >> 12) & 0xFF); k++) {
                                out.push_back((w1 & 0xFFFFFF) + k * 16);
                            }
                        }
                        else if (w0 >> 24 == 0xDE) {
                            lists.push_back(w1 & 0xFFFFFF);
                            if ((w0 >> 16) & 0xFF) break;
                        }
                        else if (w0 >> 24 == 0xDF) {
                            break;
                        }
                    }
                }
            }
        }
        std::sort(out.begin(), out.end());
        out.erase(std::unique(out.begin(), out.end()), out.end());
        return out;
    }

    void fix_rocket_windshield(uint8_t* rdram) {
        constexpr int rocket_asset = 0x31;
        constexpr int windshield_texture = 6;
        constexpr uint8_t painted = 17, unpainted = 190;
        std::vector<uint8_t> d;
        if (!rush2::assets::read_original(rdram, rocket_asset, d) || d.size() < 40) {
            return;
        }
        // G_SETTIMG (0xFD) gives the texels, G_LOADBLOCK (0xF3) their count.
        uint32_t image = 0, start = 0, bytes = 0;
        int texture = 0;
        for_each_texture_load(d, [&](uint32_t, uint32_t w0, uint32_t w1) {
            if (w0 >> 24 == 0xFD) {
                image = w1 & 0xFFFFFF;
            }
            else if (w0 >> 24 == 0xF3 && texture++ == windshield_texture) {
                start = image;
                bytes = (((w1 >> 12) & 0xFFF) + 1) * 2;
            }
        });
        if (bytes == 0 || start + bytes > d.size()) {
            return;
        }
        for (uint32_t i = start; i < start + bytes; i++) {
            if (d[i] == painted) d[i] = unpainted;
        }
        rush2::assets::replace(rdram, rocket_asset, std::move(d));
    }

    void fix_formula_suspension(uint8_t* rdram) {
        constexpr int formula_asset = 0x1D + 18;
        constexpr uint32_t wishbone_texels = 0x828;
        constexpr uint32_t mirror_s = 0x100;           // SETTILE cms bit 0 (G_TX_MIRROR)
        std::vector<uint8_t> d;
        if (!rush2::assets::read_original(rdram, formula_asset, d) || d.size() < 40) {
            return;
        }
        // The render tile (G_SETTILE, 0xF5, tile 0-6; tile 7 is the load tile) after the wishbone's G_SETTIMG.
        bool wishbone = false, fixed = false;
        for_each_texture_load(d, [&](uint32_t o, uint32_t w0, uint32_t w1) {
            if (w0 >> 24 == 0xFD) {
                wishbone = (w1 & 0xFFFFFF) == wishbone_texels;
            }
            else if (wishbone && w0 >> 24 == 0xF5 && ((w1 >> 24) & 7) != 7) {
                put32(d, o + 4, w1 | mirror_s);
                wishbone = false;
                fixed = true;
            }
        });
        if (fixed) {
            rush2::assets::replace(rdram, formula_asset, std::move(d));
        }
    }

    void fix_stallion_spoiler(uint8_t* rdram) {
        constexpr int stallion_asset = 0x1D + 14;
        constexpr int16_t spoiler_z = -80;              // only the spoiler is behind this in the TOP models
        constexpr int16_t shift_x = -2;
        std::vector<uint8_t> d;
        if (!rush2::assets::read_original(rdram, stallion_asset, d) || d.size() < 40) {
            return;
        }
        bool fixed = false;
        for (uint32_t v : model_vertices(d, "TOP")) {
            if (v + 16 > d.size() || (int16_t)(d[v + 4] << 8 | d[v + 5]) >= spoiler_z) {
                continue;
            }
            int16_t x = (int16_t)((int16_t)(d[v] << 8 | d[v + 1]) + shift_x);
            d[v] = (uint8_t)((uint16_t)x >> 8);
            d[v + 1] = (uint8_t)x;
            fixed = true;
        }
        if (fixed) {
            rush2::assets::replace(rdram, stallion_asset, std::move(d));
        }
    }
}

void rush2::fix_car_models(uint8_t* rdram) {
    fix_rocket_windshield(rdram);
    fix_formula_suspension(rdram);
    fix_stallion_spoiler(rdram);
}

// func_8005A598 at 0x8005A8F8, the wheel's z just loaded into $f4 (stored to 0xD4($sp) in the branch's delay slot):
// $a2 = wheel (2, 3 rear), $t1 = physics car. The FORMULA's rear wheels are drawn at its rear arms' hubs.
extern "C" void rush2_fix_formula_rear_wheels(uint8_t* rdram, recomp_context* ctx) {
    constexpr int type_formula = 18;
    constexpr float rear_hub_z = -85.25f / 16.0f;   // model units are 1/16 ft
    int wheel = (int16_t)ctx->r6;
    if ((wheel == 2 || wheel == 3) && MEM_BU(0x7EA, (int32_t)ctx->r9) == type_formula) {
        ctx->f4.fl = rear_hub_z;
    }
}
