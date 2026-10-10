// Fixes for bugs in the original Rush 2 (US). Each is a small hook into the game's own code; none has a toggle.
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

void rush2::fix_rocket_windshield(uint8_t* rdram) {
    constexpr int rocket_asset = 0x31;
    constexpr int windshield_texture = 6;
    constexpr uint8_t painted = 17, unpainted = 190;
    std::vector<uint8_t> d;
    if (!rush2::assets::read_original(rdram, rocket_asset, d) || d.size() < 40) {
        return;
    }
    auto be32 = [&](size_t o) { return (uint32_t)d[o] << 24 | (uint32_t)d[o + 1] << 16 | (uint32_t)d[o + 2] << 8 | d[o + 3]; };
    // The texture load lists, header words 7-8: G_SETTIMG (0xFD) gives the texels, G_LOADBLOCK (0xF3) their count.
    uint32_t image = 0;
    int texture = 0;
    for (uint32_t o = be32(28); o + 8 <= be32(32) && o + 8 <= d.size(); o += 8) {
        uint32_t w0 = be32(o), w1 = be32(o + 4);
        if (w0 >> 24 == 0xFD) {
            image = w1 & 0xFFFFFF;
        }
        else if (w0 >> 24 == 0xF3 && texture++ == windshield_texture) {
            uint32_t bytes = (((w1 >> 12) & 0xFFF) + 1) * 2;
            if (image + bytes > d.size()) {
                return;
            }
            for (uint32_t i = image; i < image + bytes; i++) {
                if (d[i] == painted) d[i] = unpainted;
            }
            rush2::assets::replace(rdram, rocket_asset, std::move(d));
            return;
        }
    }
}
