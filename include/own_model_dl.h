#ifndef __OWN_MODEL_DL_H__
#define __OWN_MODEL_DL_H__

#include <cmath>
#include <cstdint>

#include "recomp.h"

#define F3DEX_GBI_2
#include "rt64_extended_gbi.h"

// Writing small models of the port's own into RDRAM for rush2::battle_render::add_model (vertex colored, unlit,
// unfogged glows: the Fancy UFO's beams and jets, src/sfrush_ps1/fancy_ufo.cpp; the cars' roof flashers,
// src/car_flashers.cpp). Vertices in 1/16 ft.
namespace rush2::own_model {
    // An F3DEX2 vertex at `at`: white, alpha `alpha`, no texture coordinates.
    inline void put_vertex(uint8_t* rdram, uint32_t at, float x, float y, float z, uint8_t alpha) {
        MEM_W(0, (int32_t)at) = (int32_t)((uint32_t(uint16_t(int16_t(std::lround(x)))) << 16) | uint16_t(int16_t(std::lround(y))));
        MEM_W(0, (int32_t)(at + 4)) = (int32_t)(uint32_t(uint16_t(int16_t(std::lround(z)))) << 16);
        MEM_W(0, (int32_t)(at + 8)) = 0;
        MEM_W(0, (int32_t)(at + 12)) = (int32_t)(0xFFFFFF00u | alpha);
    }

    // A display list written from `at` on.
    struct Dl {
        uint8_t* rdram;
        uint32_t at;
        void cmd(uint32_t w0, uint32_t w1) {
            MEM_W(0, (int32_t)at) = (int32_t)w0;
            MEM_W(0, (int32_t)(at + 4)) = (int32_t)w1;
            at += 8;
        }
        void cmd(const GfxCommand& c) { cmd(c.values.word0, c.values.word1); }
        // F3DEX2 G_TRI2 (vertex indices in the loaded buffer).
        void tri2(int a, int b, int c, int d, int e, int f) {
            cmd(0x06000000u | uint32_t(a * 2) << 16 | uint32_t(b * 2) << 8 | uint32_t(c * 2),
                uint32_t(d * 2) << 16 | uint32_t(e * 2) << 8 | uint32_t(f * 2));
        }
        void vtx(uint32_t address, int count) {
            cmd(0x01000000u | uint32_t(count) << 12 | uint32_t(count * 2), address);
        }
        // The render state the models draw in, pushed so the models after them keep theirs: 2-cycle, no texture use,
        // the combiner SHADE x PRIM in color and alpha, no fog then G_RM_ZB_XLU_SURF2 (blended, depth tested, not
        // written), no culling (both sides show), no lighting.
        void begin() {
            GfxCommand push[3];
            gEXPushOtherMode(&push[0]);
            gEXPushCombineMode(&push[1]);
            gEXPushGeometryMode(&push[2]);
            for (const GfxCommand& c : push) cmd(c);
            cmd(0xE7000000, 0);                 // G_RDPPIPESYNC
            cmd(0xD9F8F9FF, 0x00200005);        // clear culling, lighting, texgen and fog; set ZBUFFER, SHADE, SMOOTH
            cmd(0xE3000A01, 0x00100000);        // cycle type 2-cycle
            cmd(0xE200001C, 0x0C184A50);        // render mode: no fog, then G_RM_ZB_XLU_SURF2
            cmd(0xFC41C7FF, 0xFFFFFE38);        // combiner: SHADE x PRIM (color and alpha), then passed on
        }
        void end() {
            GfxCommand pop[3];
            gEXPopOtherMode(&pop[0]);
            gEXPopCombineMode(&pop[1]);
            gEXPopGeometryMode(&pop[2]);
            cmd(0xE7000000, 0);
            for (const GfxCommand& c : pop) cmd(c);
            cmd(0xDF000000, 0);                 // G_ENDDL
        }
    };
}

#endif
