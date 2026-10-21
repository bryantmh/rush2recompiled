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
// Hide Respawning Cars (option, on by default). A computer car sitting in its wreck (user report). After a wreck's wait (race_wreck_respawn_wait_8008E40C, 3.5 s),
// respawn_place_car_80090A40 puts the burnt car back whole (flag 0x10 of +0x7F4 cleared), marks it as being put back
// (+0x80E = 1, 0x80090F7C) and starts its glide to the respawn point (+0x6C8 = 0), which
// car_wreck_steer_to_respawn_800754B4 flies over 1.5 s before clearing +0x80E (0x80075678). A human car's body node
// follows it, placed by its view's camera (car_matrix_copy_and_apply_8009ABDC). A computer car's is placed only by
// cam_update_all_players_8009D9F4 from its car state's pose (+0x0, matrix +0x48), which skips a car with +0x80E set
// (0x8009E024): the node stayed where the wreck burned, so the car sat there whole, in its own fire, for the glide,
// then jumped onto the road to blink. A gliding computer car (+0x7E8 == 1, +0x6C8 >= 0) is now hidden instead, so
// the burnt wreck vanishes and the car reappears blinking where its glide lands. The scene walk (scene_walk_8007B518,
// 0x8007B5AC) skips a node with flag 0x400 together with its children (the car's parts hang under its body node,
// car state +0xF0) and their callbacks; the visibility pass rewrites that flag every frame, so the walk is given it
// for the gliding car's body node there instead. The car is still placed along the glide from its pose, which
// follows it, so it reappears in place.
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
// Hood View Front Wheels (option, on by default). The wheel placer (carselect_preview_update_8005A598) runs once per
// wheel node per frame ($a2 = wheel 0-3, front 0 and 1) and shows or hides it through node_swap_visibility_8005A528
// (0x8005A78C): for a car with a view (car state +0x347) it shows the wheel in that view when $s0 = camera mode
// (+0x348) >= 2 and hides it there otherwise, so the bumper (0) and hood (1) cameras never draw the car's own wheels.
// The hood halves (H0_L / H0_R, 2049's CARnHOOD) are in the body's model space, as the wheels' positions are (car
// state +0xA8), so they line up. On, the open-wheel cars (FORMULA, MOUNTAIN DEW, FORMULA 1) show their front wheels in
// the hood camera.
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
//
// Breakable groups (crash on California Speed's Silicon Valley). breakable_alloc_800BB27C puts each new breakable
// (up to 0x82) in a group: one of the groups at 0x800D8FC0 (0x34 bytes: +0 class, +2 members, +4 half size, +8/+0x14/
// +0x20 center and bounds, +0x2C first, +0x30 last member) under 15 members that it fits within 200 ft of, else a new
// group at index count (0x800D97E4, u16), with no limit. The table holds 40: group 40 is the count itself, and past
// it a group's "last member" is whatever lies there, which the next member is linked through. Stock tracks stay
// under 40; Silicon Valley's 71 cones and 41+ trees spread over its streets need more, and the 41st tree's link wrote
// a breakable's address over model slot 0's name table (0x80119018), so a model lookup failed and the placement walk
// crashed in object_behavior_run_8007FFA0. The table moves to a 0x82-group copy on the recomp heap (one group per
// breakable at most): breakable_alloc (0x800BB420, 0x800BB6D0), breakable_update_all_8008BA68 (0x8008BAB8),
// callback_8005FA48 (0x8005FA68) and race_track_load_step_8008BC64's clear loop (0x8008BD34, its bound 0x28 -> 0x82
// at 0x8008BDAC) are given its address. Nothing changes on a track with 40 groups or fewer.

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "recomp.h"
#include "librecomp/addresses.hpp"
#include "assets.h"
#include "car_types.h"
#include "rush2.h"

#include "rush2_hooks.h"
#include "scene_nodes.h"

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

    constexpr uint32_t car_base = 0x800F5470;       // physics cars, 0x81C each
    constexpr uint32_t car_size = 0x81C;
    constexpr uint32_t node_table = rush2::scene_nodes::table;     // scene nodes, 0x38 each: +0 flags
    constexpr uint32_t node_size = 0x38;
    constexpr int32_t node_hidden = 0x400;          // the scene walk skips the node and its children
    constexpr uint32_t game_time = 0x80117488;      // f32 seconds
    constexpr uint32_t game_state = 0x8010C0D0;     // s32
    constexpr int32_t state_race = 3;

    // Each computer car's body node while it glides back from a wreck (-1 none), with the game time it was set at.
    struct GlideHidden {
        int32_t node = -1;
        float time = 0.0f;
        int walks = 0; // scene walks it was hidden from (logged when the glide ends)
    };
    GlideHidden glide_hidden[8];

    float game_seconds(uint8_t* rdram) {
        int32_t bits = MEM_W(0, (int32_t)game_time);
        float f;
        std::memcpy(&f, &bits, sizeof(f));
        return f;
    }

    constexpr uint32_t car_kind = 0x7E8;            // u8 in the physics car: 1 drone, 2 human
    constexpr int car_human = 2;
    std::atomic<bool> stunt_first_gear = false;
    std::atomic<bool> hide_respawning_cars = true;
    std::atomic<bool> hood_front_wheels = true;
}

void rush2::set_stunt_first_gear(bool enabled) {
    stunt_first_gear.store(enabled, std::memory_order_relaxed);
}

void rush2::set_hide_respawning_cars(bool enabled) {
    hide_respawning_cars.store(enabled, std::memory_order_relaxed);
}

void rush2::set_hood_front_wheels(bool enabled) {
    hood_front_wheels.store(enabled, std::memory_order_relaxed);
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

// cam_update_all_players_8009D9F4 at 0x8009E028 (for each active computer car with a body node): $v0 = physics car,
// $v1 = car state, $t3 = +0x80E (being put back), which skips placing the body node.
extern "C" void rush2_fix_drone_glide_node(uint8_t* rdram, recomp_context* ctx) {
    int32_t car = (int32_t)ctx->r2;
    int i = (int)(((uint32_t)car - car_base) / car_size);
    if (i < 0 || i >= 8) {
        return;
    }
    if (hide_respawning_cars.load(std::memory_order_relaxed) && MEM_BU(0x7E8, car) == 1 &&
        (int16_t)MEM_H(0x6C8, car) >= 0) {
        glide_hidden[i].node = MEM_W(0xF0, (int32_t)ctx->r3);
        glide_hidden[i].time = game_seconds(rdram);
        ctx->r11 = 0;
    } else if (glide_hidden[i].node >= 0) {
        fprintf(stderr, "[Wreck] car %d hidden through its respawn glide (%d scene walks)\n", i, glide_hidden[i].walks);
        glide_hidden[i] = {};
    }
}

// scene_walk_8007B518 at 0x8007B5AC (each sibling node): $s7 = node, $t1 = its flags, skipped with 0x400 set.
extern "C" void rush2_fix_drone_glide_hide(uint8_t* rdram, recomp_context* ctx) {
    if (MEM_W(0, (int32_t)game_state) != state_race) {
        return;
    }
    int32_t node = (int32_t)(((uint32_t)ctx->r23 - node_table) / node_size);
    float now = game_seconds(rdram);
    for (GlideHidden& g : glide_hidden) {
        // Set within the last few frames (game time stands still while paused, which keeps it set).
        if (g.node == node && now - g.time >= 0.0f && now - g.time < 0.1f) {
            ctx->r9 |= node_hidden;
            g.walks++;
        }
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

    // Hood View Front Wheels: the FORMULA's front arms, taken from its front panel for the hood camera's hood list.
    struct FormulaArms {
        bool ok = false;
        uint32_t hood_lists[2] = {};        // file offsets of H0_L's and H2_L's LOD 0 lists
        uint32_t texture_list = 0;          // the wishbone's texture load list
        std::vector<uint32_t> commands;     // its render state and triangles (w0, w1 pairs), indices from vertex 0
        std::vector<uint32_t> vertices;     // file offsets of the triangles' vertices, in their new order
        uint32_t scratch = 0;               // room at the copy's end for the hood lists with the arms (loaded with it)
    };
    FormulaArms formula_arms;
    constexpr uint32_t arms_list_size = 0x100;      // per hood half: the wrapper list, then its vertices
    constexpr uint32_t car_slot_size = 0x8001CF28;  // largest car asset: every car slot is allocated at it

    int32_t find_model(const std::vector<uint8_t>& d, const std::string& name) {
        uint32_t names = be32(d, 4), count = be32(d, 16);
        for (uint32_t m = 0; m < count && names + m * 0x18 + 16 <= d.size(); m++) {
            if (strncmp(reinterpret_cast<const char*>(&d[names + m * 0x18]), name.c_str(), 16) == 0) return (int32_t)m;
        }
        return -1;
    }

    // In FORM1D0_FR1's LOD 0 list, the commands after the G_DL to the wishbone's texture load list (G_SETTIMG of
    // wishbone_texels) up to the next G_DL / G_VTX / G_ENDDL: G_SETOTHERMODE_L, G_SETCOMBINE and the front arms'
    // G_TRI2s (vertices 14-19 of the list's first G_VTX: a tent across the car, x -38 to 38 at z 54-63).
    void capture_formula_arms(const std::vector<uint8_t>& d, uint32_t wishbone_texels) {
        uint32_t models = be32(d, 0);
        int32_t h0 = find_model(d, "FORM1H0_L"), h2 = find_model(d, "FORM1H2_L"), fr = find_model(d, "FORM1D0_FR1");
        if (h0 < 0 || h2 < 0 || fr < 0) return;
        FormulaArms a;
        a.hood_lists[0] = be32(d, models + h0 * 0x34 + 12) & 0xFFFFFF;
        a.hood_lists[1] = be32(d, models + h2 * 0x34 + 12) & 0xFFFFFF;
        uint32_t vtx = 0, vtx_first = 0;
        bool in_arms = false;
        std::vector<int> used(64, -1);
        int next = 0;
        for (uint32_t o = be32(d, models + fr * 0x34 + 12) & 0xFFFFFF; o + 8 <= d.size(); o += 8) {
            uint32_t w0 = be32(d, o), w1 = be32(d, o + 4), op = w0 >> 24;
            if (op == 0xDF) break;
            if (op == 0x01) {
                if (in_arms) break;
                vtx = w1 & 0xFFFFFF;
                vtx_first = ((w0 & 0xFF) >> 1) - ((w0 >> 12) & 0xFF);
                continue;
            }
            if (op == 0xDE) {
                if (in_arms) break;
                uint32_t list = w1 & 0xFFFFFF;
                if (list + 8 <= d.size() && be32(d, list) >> 24 == 0xFD && (be32(d, list + 4) & 0xFFFFFF) == wishbone_texels) {
                    in_arms = true;
                    a.texture_list = list;
                }
                continue;
            }
            if (!in_arms) continue;
            if (op == 0x05 || op == 0x06) {
                // Renumber the triangles' vertices (index * 2 in bytes 1-3 and, for G_TRI2, 5-7) from 0.
                uint32_t words[2] = { w0, w1 };
                for (int k = 0; k < (op == 0x06 ? 2 : 1); k++) {
                    for (int b = 0; b < 3; b++) {
                        int shift = 16 - b * 8;
                        int v = int((words[k] >> shift) & 0xFF) / 2 - (int)vtx_first;
                        if (v < 0 || v >= 64) return;
                        if (used[v] < 0) {
                            used[v] = next++;
                            uint32_t at = vtx + uint32_t(v) * 16;
                            if (at + 16 > d.size()) return;
                            a.vertices.push_back(at);
                        }
                        words[k] = (words[k] & ~(0xFFu << shift)) | (uint32_t(used[v] * 2) << shift);
                    }
                }
                a.commands.push_back(words[0]);
                a.commands.push_back(words[1]);
            }
            else if (op == 0xE2 || op == 0xFC) {
                a.commands.push_back(w0);
                a.commands.push_back(w1);
            }
        }
        a.ok = a.texture_list != 0 && !a.vertices.empty() && next <= 32;
        if (a.ok) formula_arms = std::move(a);
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
        capture_formula_arms(d, wishbone_texels);
        if (formula_arms.ok) {
            // The wrapper lists are built in the loaded car (the game heap, which the renderer reaches), in room added
            // at the copy's end. Every car slot is allocated at the largest car asset's size.
            formula_arms.scratch = (uint32_t)(d.size() + 15) & ~15u;
            d.resize(formula_arms.scratch + 2 * arms_list_size, 0);
            if (d.size() > (uint32_t)MEM_W(0, (int32_t)car_slot_size)) {
                MEM_W(0, (int32_t)car_slot_size) = (int32_t)((d.size() + 15) & ~15u);
            }
            fixed = true;
        }
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

namespace {
    constexpr int type_formula = 18, type_formula1 = rush2::car_types::first_2049; // type_dew: above
    constexpr int hood_camera = 1;
    constexpr uint32_t player_count = 0x8010C3E2;   // s16; the hood is drawn with one player
    constexpr uint32_t model_slots = 0x80118D78;    // per model slot: its records (8 bytes each)
    constexpr uint32_t model_record_size = 0x34;
    constexpr uint32_t car_part_handles = rush2::car_types::part_handles; // + car * 0x4E: u16 per part
    constexpr int hood_parts[2] = { 31, 32 };      // H0_L, H2_L (car_hood_update_8005BFD8 swaps by damage)

    // How far the hood camera's hood is modeled from the body's middle, in model units (1/16 ft): the camera sits
    // 0.55 ft to the side (0x800BFC60), and the open-wheel cars' hoods were shifted with it so the nose looks centered.
    // FORM1H0_L / H0_R meet at x = -6 (x -41 to 28), DEWH0_L / H0_R at -8 (-40 to 24); 2049's CAR1HOOD's nose tip is
    // at x = -8. The body (and wheels) are centered on x = 0.
    int hood_shift(int type) {
        return type == type_formula ? -6 : type == type_dew || type == type_formula1 ? -8 : 0;
    }

    bool open_wheel(int type) {
        return type == type_formula || type == type_dew || type == type_formula1;
    }

    // The hood half's list with the FORMULA's front arms after it, per race car and hood part, in the loaded car.
    struct ArmsList {
        uint32_t address = 0;   // the wrapper list
        uint32_t original = 0;  // the record's own list it calls
    };
    ArmsList arms_lists[8][2];

    uint32_t record_of(uint8_t* rdram, int car, int part) {
        uint16_t h = MEM_HU(0, (int32_t)(car_part_handles + uint32_t(car) * 0x4E + part * 2));
        if (h == 0) return 0;
        uint32_t base = (uint32_t)MEM_W(0, (int32_t)(model_slots + uint32_t(h >> 10) * 8));
        return base == 0 ? 0 : base + uint32_t(h & 0x3FF) * model_record_size;
    }

    // Builds car's wrapper for hood part i over list (the record's relocated LOD 0 list, at the hood list's file
    // offset): G_DL list, then the arms: G_VTX of their vertices (moved by the hood's shift), G_TEXTURE on, G_DL to the
    // wishbone's texture load list, and the front panel's render state and triangles.
    void build_arms_list(uint8_t* rdram, int car, int i, uint32_t list, int shift) {
        ArmsList& a = arms_lists[car][i];
        // The relocator adds the container's load address to file offsets; the hood list gives it.
        uint32_t base = list - formula_arms.hood_lists[i];
        a.address = base + formula_arms.scratch + uint32_t(i) * arms_list_size;
        // Inside a list the game's pointers are physical (the record holds the KSEG0 address).
        auto addr = [](uint32_t at) { return at & 0x1FFFFFFF; };
        uint32_t n = (uint32_t)formula_arms.vertices.size();
        uint32_t vertices = a.address + arms_list_size - n * 16;
        // The loaded container's own vertices (as its load left them), x moved by the hood's shift.
        for (uint32_t k = 0; k < n; k++) {
            uint32_t from = base + formula_arms.vertices[k];
            uint32_t x = vertices + k * 16;
            for (uint32_t b = 0; b < 16; b += 4) {
                MEM_W(0, (int32_t)(x + b)) = MEM_W(0, (int32_t)(from + b));
            }
            MEM_H(0, (int32_t)x) = (int16_t)(MEM_H(0, (int32_t)x) + shift);
        }
        std::vector<uint32_t> cmds = {
            0xDE000000, addr(list),
            0x01000000 | (n << 12) | (n * 2), addr(vertices),
            0xD7000002, 0xFFFFFFFF,
            0xDE000000, addr(base + formula_arms.texture_list),
        };
        cmds.insert(cmds.end(), formula_arms.commands.begin(), formula_arms.commands.end());
        cmds.push_back(0xDF000000);
        cmds.push_back(0);
        for (size_t k = 0; k < cmds.size() && k * 4 + n * 16 < arms_list_size; k++) {
            MEM_W(0, (int32_t)(a.address + uint32_t(k) * 4)) = (int32_t)cmds[k];
        }
        a.original = list;
    }

    // Each frame, for a race car with a view: points its hood halves' LOD 0 list at the wrapper while the option is on
    // and the car is the FORMULA, and back at their own list otherwise. A new race's car load writes the record anew.
    void update_formula_arms(uint8_t* rdram, int car, int type) {
        if (car < 0 || car >= 8 || !formula_arms.ok) return;
        bool want = hood_front_wheels.load(std::memory_order_relaxed) && type == type_formula;
        for (int i = 0; i < 2; i++) {
            uint32_t record = record_of(rdram, car, hood_parts[i]);
            if (record == 0) continue;
            uint32_t list = (uint32_t)MEM_W(0xC, (int32_t)record);
            ArmsList& a = arms_lists[car][i];
            if (a.address != 0 && list == a.address) {
                if (!want) MEM_W(0xC, (int32_t)record) = (int32_t)a.original;
                continue;
            }
            if (want) {
                build_arms_list(rdram, car, i, list, hood_shift(type));
                MEM_W(0xC, (int32_t)record) = (int32_t)a.address;
            }
        }
    }

    // The hood (and so the wheels' shift) is drawn in camera mode 1 with one player.
    bool hood_drawn(uint8_t* rdram, uint32_t car_state, int view) {
        return view >= 0 && MEM_B(0x348, (int32_t)car_state) == hood_camera && MEM_H(0, (int32_t)player_count) < 2;
    }
}

// func_8005A598 at 0x8005A774, $s0 just set to camera mode >= 2 (shown in the car's own view): $a0 = car state,
// $a2 = wheel (0, 1 front), $t0 = node record (+0xE car), $t1 = physics car, $t3 = the car's view (-1 none).
// Hood View Front Wheels: shows the open-wheel cars' front wheels in the hood camera, and has the FORMULA's hood list
// draw its front arms.
extern "C" void rush2_hood_front_wheels(uint8_t* rdram, recomp_context* ctx) {
    int wheel = (int16_t)ctx->r6;
    int view = (int8_t)ctx->r11;
    int type = MEM_BU(0x7EA, (int32_t)ctx->r9);
    if (wheel == 0 && view >= 0) {
        update_formula_arms(rdram, MEM_H(0xE, (int32_t)ctx->r8), type);
    }
    if (!hood_front_wheels.load(std::memory_order_relaxed) || (wheel != 0 && wheel != 1) || view < 0 ||
        MEM_B(0x348, (int32_t)ctx->r4) != hood_camera || !open_wheel(type)) {
        return;
    }
    ctx->r16 = 1;
}

// func_8005A598 at 0x8005A8F4, the wheel's x stored at 0xCC($sp): the open-wheel cars' front wheels move with the hood
// (hood_shift) while it is drawn, so they sit beside its nose.
extern "C" void rush2_hood_front_wheels_x(uint8_t* rdram, recomp_context* ctx) {
    int wheel = (int16_t)ctx->r6;
    int type = MEM_BU(0x7EA, (int32_t)ctx->r9);
    if (!hood_front_wheels.load(std::memory_order_relaxed) || (wheel != 0 && wheel != 1) || !open_wheel(type) ||
        !hood_drawn(rdram, (uint32_t)ctx->r4, (int8_t)ctx->r11)) {
        return;
    }
    uint32_t at = (uint32_t)ctx->r29 + 0xCC;
    int32_t bits = MEM_W(0, (int32_t)at);
    float x;
    std::memcpy(&x, &bits, sizeof(x));
    x += hood_shift(type) / 16.0f;
    std::memcpy(&bits, &x, sizeof(x));
    MEM_W(0, (int32_t)at) = bits;
}

// Breakable groups: the 0x82-group copy of the table at 0x800D8FC0 (see the top of the file). Each hook runs right
// after its function loads the table's address into a register and puts the copy's there.
namespace {
    constexpr uint32_t breakable_group_size = 0x34, breakable_groups = 0x82;
    uint32_t breakable_group_table(uint8_t* rdram) {
        static uint32_t table = 0;
        if (table == 0) {
            void* p = recomp::alloc(rdram, breakable_group_size * breakable_groups);
            memset(p, 0, breakable_group_size * breakable_groups);
            table = (uint32_t)((uint8_t*)p - rdram) + 0x80000000;
        }
        return table;
    }
}

extern "C" void rush2_breakable_groups_s3(uint8_t* rdram, recomp_context* ctx) {
    ctx->r19 = (int32_t)breakable_group_table(rdram);
}
extern "C" void rush2_breakable_groups_s6(uint8_t* rdram, recomp_context* ctx) {
    ctx->r22 = (int32_t)breakable_group_table(rdram);
}
extern "C" void rush2_breakable_groups_t8(uint8_t* rdram, recomp_context* ctx) {
    ctx->r24 = (int32_t)breakable_group_table(rdram);
}
extern "C" void rush2_breakable_groups_a2(uint8_t* rdram, recomp_context* ctx) {
    ctx->r6 = (int32_t)breakable_group_table(rdram);
}
