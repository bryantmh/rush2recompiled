// Rush 2049's stunt scoring (include/stunt2049.h), with the Stunt Scoring option on Rush 2049.
//
// A port of Rush 2049 (N64 USA)'s stunt code, docs/rush2049_research/stunts.md: func_800C2BE0 (each player's trick
// detectors, run every frame in a stunt round) with func_800C2944 (rolls), func_800C26C4 (helicopter spins),
// func_800C2430 (flips), func_800C220C and func_800C2004 (front and back wheels), func_800C1B60 (a trick, or the
// landing that scores the combo) and func_800C3578 (a crash, which drops the combo). Rush 2049's per-player state is
// a detector (0x801569B8 + player x 0x7C, `Detector`) and a score (0x80152038 + player x 0x78, `Score`).
//
// It runs in place of Rush 2's scoring (car_stunt_scoring_update_80065588, once per physics tick per player) and
// reads Rush 2's car (0x800F5470 + car x 0x81C), which has Rush 2049's fields at other offsets:
//   2049 car +0x4C angular velocity, rad/s, body frame (x pitch, y yaw, z roll)  ->  Rush 2 +0x40
//   2049 car +0x5EC wheel heights above the ground (f32[4]; 0, 1 the front)       ->  Rush 2 +0x5DC
//   2049 car +0x61C wheel materials (u16[4], 8 airborne)                         ->  Rush 2 +0x60C
//   2049 car +0x6C4 respawn (s16, -1 once placed)                                ->  Rush 2 +0x6C8
//   2049 car +0x3F8 wings out (func_800E15A0)                                    ->  rush2::wings::gliding
//   2049 car state +0x358 wrecked (s8)                                           ->  Rush 2 car +0x648
//   2049 car +0x7D4 bit 0x100000, the body pushed out of the world (func_800C3880) -> body_hit, set from Rush 2's
//       car_wall_collision_query_8006FC94 at the same push (0x8006FF60)
// Rush 2049 adds a frame's rotation (car +0x2E0 += angular velocity x dt, func_800D0424) and runs the detectors once a
// frame; here each physics tick turns its own step. Times are the game clock (2049 0x801543CC, Rush 2 0x80117488).
//
// Rush 2049 shows the tricks as icons (file 67, STNT_*) sliding in with their counts (mode overlay func_80391B00);
// here a scored combo shows on Rush 2's stunt panel (hud_widget_text_cb_player_stats_800B7654): the trick's name and
// count, or COMBO, over its points times its multiplier, and BEST and TOTAL are Rush 2049's. Rush 2's total (u16)
// and best (u8) get the scores too, clamped, for its post-race and records; the profile's BEST STUNT (a byte) takes
// the best clamped to 255 and its trick counts take Rush 2049's flips, rolls, two-wheel and air tricks.
//
// Not ported: Rush 2049's end of a stunt round (it waits until no player has had a trick going for 2.69 s,
// 0x8016139C / 0x80152738); Rush 2's clock ends the round.

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "recomp.h"
#include "librecomp/addresses.hpp"

#include "rush2_hooks.h"
#include "rush2.h"
#include "font2049.h"
#include "hud_layout.h"
#include "rush2049_rom.h"
#include "players4.h"
#include "online.h"
#include "online_sync.h"
#include "stunt2049.h"
#include "wings.h"

extern "C" void record_stat_stunt_max_80065360(uint8_t* rdram, recomp_context* ctx);
extern "C" void record_stat_stunt_count_8006544C(uint8_t* rdram, recomp_context* ctx);

namespace {
    std::atomic<bool> scoring_2049 = false;

    // Rush 2's car (physics) structs.
    constexpr uint32_t cars = 0x800F5470;
    constexpr uint32_t car_size = 0x81C;
    constexpr uint32_t car_angular_velocity = 0x40;  // f32[3], rad/s, body frame: x pitch, y yaw, z roll
    constexpr uint32_t car_wheel_height = 0x5DC;     // f32[4], height of each wheel above the ground
    constexpr uint32_t car_wheel_material = 0x60C;   // u16[4], 8 = in the air
    constexpr uint32_t car_wrecked = 0x648;          // s8
    constexpr uint32_t car_respawn = 0x6C8;          // s16: -1 once placed
    constexpr uint32_t car_dt = 0x718;               // f32, the physics step
    constexpr int max_cars = 8;
    constexpr uint16_t material_air = 8;

    constexpr uint32_t game_time = 0x80117488;       // f32 seconds

    // Rush 2's stunt panel, per player (moved by src/players4.cpp).
    constexpr uint32_t hud_trick_shown = 0x8010C070; // s8: the trick name shows
    constexpr uint32_t hud_trick_name = 0x8010C0D4;  // u8: its name, 0-4 of the table below
    constexpr uint32_t hud_trick_time = 0x8010C0D8;  // f32: when it was scored (shown for 2.75 s)
    constexpr uint32_t hud_best = 0x8010C068;        // u8
    constexpr uint32_t hud_total = 0x8010C074;       // u16
    constexpr uint32_t trick_names = 0x800C4BAC;     // char*[5] per language: 2 WHEELS, AIR TIME, FLIPS, ROLLS, COMBO
    constexpr uint32_t language = 0x800C4618;
    constexpr int combo_name = 4;

    constexpr int max_players = rush2::online::max_players;

    // Rush 2049's tricks (func_800C1B60's argument; its icons 0x80393B40, STNT_*).
    enum Trick {
        flip_back,          // STNT_FLIP_B: half a turn about x, positive
        flip_front,         // STNT_FLIP_F
        roll_left,          // STNT_ROLL_L: half a turn about z, negative
        roll_right,         // STNT_ROLL_R
        heli,               // STNT_HELI: half a turn about y
        multi,              // STNT_MULTI: a turn spread over two or three axes
        two_wheels_side,    // STNT_2WHL_S: one side's wheels down, per 0.15 s after 0.25 s
        two_wheels_back,    // STNT_2WHL_B: only the back wheels down
        two_wheels_front,   // STNT_2WHL_F: only the front wheels down
        air,                // STNT_AIR: per second in the air over 5
        trick_count,
        land = 11,          // scores the combo
    };
    // Rush 2049's names for them (its trick name table, English).
    const char* const trick_text[trick_count] = {
        "BACK FLIP", "FRONT FLIP", "LEFT ROLL", "RIGHT ROLL", "SPIN", "TWIST", "2 WHEEL", "WHEELIE", "ENDO", "AIR TIME",
    };

    // Rush 2049's values (0x80123EE4-0x80123F50, func_800C1B60's tables).
    constexpr float wheels_min_time = 0.25f;  // two-wheel tricks start counting after this
    constexpr float wheels_step = 0.15f;      // and count once per this
    constexpr float axis_slip = 0.471238911f; // 27 degrees: the other axes' turn that ends a flip, spin or roll
    constexpr float roll_pitch_slip = 1.25663709f; // 72 degrees: the pitch that ends a roll
    constexpr float half_turn_low = 2.51327419f;   // a half turn counts between 144
    constexpr float half_turn_high = 3.76991129f;  // and 216 degrees,
    constexpr float half_turn_cut = 1.72787595f;   // or past 99 degrees when cut short
    constexpr float pi = 3.14159274f;
    constexpr float air_min_time = 5.0f;      // air time counts past 5 s, once a second
    constexpr float land_delay = 1.0f;        // the combo scores 1 s after the car is back on its wheels
    constexpr float clean_wings = 1.0f;       // a pure flip, roll or spin combo with wings out at most this long
    constexpr float perfect_wings = 0.0f;
    constexpr float good_wings = 3.0f;
    constexpr float shown_time = 2.69f;       // the overlay shows a combo this long (0x80394F4C)

    // Detector flags (2049 detector +0).
    enum : uint32_t {
        det_side = 0x1,           // two wheels, one side
        det_front = 0x2,          // front wheels only (trick 8)
        det_back = 0x4,           // back wheels only (trick 7)
        det_heli = 0x10,
        det_roll = 0x20,
        det_flip = 0x40,
        det_multi = 0x80,
        det_air = 0x100,
        det_tricks = 0x1F7,       // a trick going
        det_spins = 0x1F0,        // a turn or air going
        det_x_pos = 0x01000000, det_x_neg = 0x02000000,
        det_y_pos = 0x04000000, det_y_neg = 0x08000000,
        det_z_pos = 0x10000000, det_z_neg = 0x20000000,
        det_any_but_signs = 0xC08FFFFF,
        det_reset = 0x15000000,
    };

    struct Detector {          // 2049: 0x801569B8 + player * 0x7C
        uint32_t flags = det_reset;
        float roll[3] = {};    // +4: turn about x, y, z since the roll started
        float flip[3] = {};    // +0x10
        float spin[3] = {};    // +0x1C
        float multi[3] = {};   // +0x28: absolute turns
        float air_start = 0.0f, air_end = 0.0f;       // +0x34
        float side_start = 0.0f, side_end = 0.0f;     // +0x3C
        float front_start = 0.0f, front_end = 0.0f;   // +0x44
        float back_start = 0.0f, back_end = 0.0f;     // +0x4C
        float settled = 0.0f;                         // +0x54: when the car came down with a combo going
    };

    struct Score {             // 2049: 0x80152038 + player * 0x78
        bool active = false;   // +0: the overlay shows tricks
        bool crashed = false;  // +2: the combo was lost to a wreck
        float time = 0.0f;     // +4: last trick
        int32_t best = 0;      // +8
        int32_t points = 0;    // +0xC: the combo's points before the multiplier
        int32_t shown_points = 0; // +0x10: the last combo's score
        int32_t total = 0;     // +0x14
        int32_t changes = 0;   // +0x18: tricks done in a row of a different kind than the last
        // +0x1C: the last trick. Rush 2049 zeroes it at the round's start (func_800FB2C8), so a back flip (trick 0)
        // as the round's first trick didn't start a combo; it starts as after a combo (the landing, 11).
        int32_t last = land;
        std::array<int16_t, trick_count> count{};       // +0x20
        std::array<int16_t, trick_count> shown_count{}; // +0x34
        std::array<int16_t, trick_count> all_count{};   // +0x48: the round's
        float wings_time = 0.0f;  // +0x60: seconds with wings out during the tricks
        int8_t landing = 0;    // +0x64: 1 no wings (x7 more), 2 wings under 3 s (x2 more)
        int8_t shown_landing = 0; // +0x65: the last combo's
        int8_t bonus = 0;      // +0x66: 0xB pure spins, 0xC pure rolls, 0xD pure flips, 3-10 kinds of trick
        int8_t shown_bonus = 0;   // +0x67: the last combo's
        // +0x68: 1 after every combo. Rush 2049 zeroes it at the round's start, which gave the round's first combo
        // one less; it starts at 1.
        int32_t mult = 1;
        int32_t shown_mult = 1;   // +0x6C: the multiplier before the landing's bonuses ("BONUS X n")
        int32_t new_kinds = 0; // +0x70: new kinds of trick the HUD hasn't flashed "BONUS X n" for
        bool landed = false;   // +0x74: the combo has landed (shown_* are the landing's)

        // The port's: what Rush 2's panel shows of the last scored combo.
        std::string shown_name;
        int32_t shown_base = 0;
        int32_t panel_mult = 0;
    };

    struct Player {
        Detector det;
        Score score;
    };
    std::array<Player, max_players> players;

    std::array<bool, max_cars> body_hit{};  // 2049 car +0x7D4 & 0x100000, cleared by the detectors
    // Each car's player, from the scoring's calls (-1 none).
    std::array<int, max_cars> car_player = [] { std::array<int, max_cars> a; a.fill(-1); return a; }();

    // RUSH2_STUNT_LOG=1 prints every trick and scored combo (test aid).
    const bool log_tricks = std::getenv("RUSH2_STUNT_LOG") != nullptr;

    // Rush 2's stunt panel strings (rush2_stunt2049_hud).
    uint32_t text_scratch = 0;
    constexpr uint32_t text_slot = 32;
    uint32_t saved_name = 0;
    uint32_t patched_slot = 0;
    std::string printed_name[max_players];  // RUSH2_STUNT_LOG: the trick name the panel printed

    float& f32(uint8_t* rdram, uint32_t addr) {
        return *reinterpret_cast<float*>(&MEM_W(0, (int32_t)addr));
    }

    uint32_t car_addr(int car) {
        return cars + (uint32_t)car * car_size;
    }

    int32_t call(uint8_t* rdram, recomp_context* ctx, void (*func)(uint8_t*, recomp_context*), int32_t a0, int32_t a1) {
        recomp_context saved = *ctx;
        ctx->r4 = a0;
        ctx->r5 = a1;
        func(rdram, ctx);
        int32_t ret = (int32_t)ctx->r2;
        *ctx = saved;
        return ret;
    }

    // Rush 2049's per-frame globals for the player being run (0x80157238 and the others).
    struct Frame {
        int down = 0;          // wheels on the ground (0x80157238)
        int front_down = 0;    // wheels 0 and 1 (0x8015F730)
        int back_down = 0;     // wheels 2 and 3 (0x8015F728)
        int left_down = 0;     // wheels 0 and 2 (0x8015B248)
        int right_down = 0;    // wheels 1 and 3 (0x8015B258)
        bool can_trick = false;     // 0x80161360
        float rate[3] = {};         // |turn this step| (0x80161388)
        bool reversed[3] = {};      // the turn changed direction this step (0x80156BC8, 0x80156BD8, 0x80156CE4)
        bool hit = false;           // the body hit the world
        float step[3] = {};         // turn this step (2049 car state +0x368)
        float height[4] = {};
        float now = 0.0f;
    };

    void reset_detector(Detector& d) {
        d = Detector{};
        d.flags |= det_reset;
    }

    // func_800C1B60(trick), $t1 = the player.
    void award(uint8_t* rdram, recomp_context* ctx, int player, int trick, float now) {
        Player& pl = players[player];
        Score& s = pl.score;
        s.landed = trick == land;
        s.active = true;
        s.time = now;
        if (trick != land) {
            if (s.count[trick] == 0 && s.changes > 0) {
                s.mult++;
                s.new_kinds++;
            }
            s.count[trick]++;
        }
        if (trick != s.last) {
            s.last = trick;
            s.changes++;
        }
        switch (trick) {
            case multi:
            case two_wheels_side:
                s.points += 3;
                break;
            case land:
                break;
            default:
                s.points += 5;
                break;
        }
        s.shown_mult = s.mult;
        if (trick != land) {
            if (log_tricks) {
                std::fprintf(stderr, "[stunt2049] p%d %.2f %s points %d mult %d\n", player, now, trick_text[trick],
                             s.points, s.mult);
            }
            return;
        }

        int base = s.points;
        bool scored = !s.crashed;
        int kinds = 0;
        if (scored) {
            for (int i = 0; i < trick_count; i++) {
                if (s.count[i] > 0) {
                    kinds++;
                }
                s.all_count[i] += s.count[i];
            }
            bool turns = s.count[flip_back] || s.count[flip_front] || s.count[roll_left] || s.count[roll_right] ||
                         s.count[heli];
            if (turns && s.wings_time <= clean_wings && (kinds == 1 || (kinds == 2 && s.count[air] != 0))) {
                if (s.count[heli]) {
                    s.bonus = 0xB;
                    s.mult += 15;
                } else if (s.count[flip_back] || s.count[flip_front]) {
                    s.bonus = 0xD;
                    s.mult += 25;
                } else {
                    s.bonus = 0xC;
                    s.mult += 25;
                }
            } else if (s.count[air] || s.count[multi] || turns) {
                if (s.wings_time == perfect_wings) {
                    s.landing = 1;
                    s.mult += 7;
                } else if (s.wings_time <= good_wings) {
                    s.landing = 2;
                    s.mult += 2;
                }
            }
            if (kinds >= 3) {
                static constexpr int kinds_bonus[] = { 10, 20, 40, 60, 80, 100, 500, 1000 };
                s.bonus = (int8_t)kinds;
                s.mult += kinds_bonus[std::min(kinds, 10) - 3];
            }
            if (s.mult >= 2) {
                s.points *= s.mult;
            }
            s.total += s.points;
            s.best = std::max(s.best, s.points);
        }

        if (log_tricks) {
            std::fprintf(stderr,
                         "[stunt2049] p%d %.2f land %s: %d x %d = %d (kinds %d, bonus %d, landing %d, wings %.2f s) "
                         "total %d\n",
                         player, now, scored ? "scored" : "crashed", base, s.mult, s.points, kinds, s.bonus, s.landing,
                         s.wings_time, s.total);
        }

        // The port's: show the combo on Rush 2's panel and keep Rush 2's totals and records.
        if (scored && base > 0) {
            int single = -1;
            for (int i = 0; i < trick_count; i++) {
                if (s.count[i] > 0) {
                    single = kinds == 1 ? i : -1;
                }
            }
            if (single >= 0) {
                s.shown_name = trick_text[single];
                if (s.count[single] > 1) {
                    s.shown_name += " X" + std::to_string(s.count[single]);
                }
            } else {
                s.shown_name.clear();   // COMBO, Rush 2's own name
            }
            s.shown_base = base;
            s.panel_mult = std::max(s.mult, 1);
            MEM_B(player, (int32_t)rush2::players4::moved(hud_trick_shown)) = 1;
            MEM_B(player, (int32_t)rush2::players4::moved(hud_trick_name)) = combo_name;
            f32(rdram, rush2::players4::moved(hud_trick_time) + (uint32_t)player * 4) = now;
            MEM_H(player * 2, (int32_t)rush2::players4::moved(hud_total)) = (int16_t)std::min(s.total, 0xFFFF);
            MEM_B(player, (int32_t)rush2::players4::moved(hud_best)) = (int8_t)std::min(s.best, 0xFF);
            // Rush 2's profile stats (record_stats): BEST STUNT is a byte; 2 WHEELS, AIR TIME, FLIPS and ROLLS count.
            call(rdram, ctx, record_stat_stunt_max_80065360, player, std::min(s.points, 0xFF));
            auto count = [&](int stat, int n) {
                for (int i = 0; i < n; i++) {
                    call(rdram, ctx, record_stat_stunt_count_8006544C, player, stat);
                }
            };
            count(0, s.count[two_wheels_side] + s.count[two_wheels_back] + s.count[two_wheels_front]);
            count(1, s.count[air]);
            count(2, s.count[flip_back] + s.count[flip_front]);
            count(3, s.count[roll_left] + s.count[roll_right]);
        }

        s.shown_bonus = s.bonus;
        s.shown_landing = s.landing;
        s.shown_count = s.count;
        s.count.fill(0);
        s.shown_points = s.points;
        s.bonus = 0;
        s.landing = 0;
        s.mult = 1;
        s.new_kinds = 0;
        s.changes = 0;
        s.points = 0;
        s.wings_time = 0.0f;
        reset_detector(pl.det);
    }

    // func_800C3578: a wreck drops the combo.
    void crash(uint8_t* rdram, recomp_context* ctx, int player, float now) {
        Player& pl = players[player];
        pl.score.crashed = true;
        if (pl.score.changes != 0) {
            award(rdram, ctx, player, land, now);
        }
        reset_detector(pl.det);
    }

    // Counts a timed trick once per wheels_step past wheels_min_time (func_800C2004, func_800C220C, two wheels).
    void award_timed(uint8_t* rdram, recomp_context* ctx, int player, int trick, float start, float end, float now) {
        float t = end - start;
        if (t > wheels_min_time) {
            for (t -= wheels_min_time; t > 0.0f; t -= wheels_step) {
                award(rdram, ctx, player, trick, now);
            }
        }
    }

    // func_800C2944 (rolls, z), func_800C26C4 (spins, y), func_800C2430 (flips, x): a half turn about the axis whose
    // turn was the fastest when the car left the ground, ended by a turn of the others.
    void turn(uint8_t* rdram, recomp_context* ctx, int player, const Frame& f, int axis, uint32_t flag, float* acc) {
        Detector& d = players[player].det;
        int a = (axis + 1) % 3, b = (axis + 2) % 3;
        if (d.flags & flag) {
            if (f.reversed[axis] || f.down >= 3) {
                d.flags &= ~flag;
            } else {
                for (int i = 0; i < 3; i++) {
                    acc[i] += f.step[i];
                }
            }
            float abs_axis = std::fabs(acc[axis]);
            // The other axes' limits: rolls allow more pitch (x).
            float limit_a = axis == 2 && a == 0 ? roll_pitch_slip : axis_slip;
            float limit_b = axis == 2 && b == 0 ? roll_pitch_slip : axis_slip;
            if (std::fabs(acc[a]) > limit_a || std::fabs(acc[b]) > limit_b) {
                d.flags &= ~flag;
                return;
            }
            if ((half_turn_low < abs_axis && abs_axis < half_turn_high) ||
                ((f.reversed[axis] || f.down > 0 || f.hit) && half_turn_cut < abs_axis)) {
                d.flags &= ~det_multi;
                int trick = axis == 0 ? (acc[0] > 0.0f ? flip_back : flip_front)
                          : axis == 1 ? heli
                          : (acc[2] > 0.0f ? roll_right : roll_left);
                award(rdram, ctx, player, trick, f.now);
                acc[axis] += acc[axis] > 0.0f ? -pi : pi;
                acc[a] = 0.0f;
                acc[b] = 0.0f;
            }
        } else if (f.can_trick && f.rate[a] < f.rate[axis] && f.rate[b] < f.rate[axis]) {
            d.flags |= flag;
            for (int i = 0; i < 3; i++) {
                acc[i] = f.step[i];
            }
        }
    }

    // func_800C2BE0 for one player.
    void update(uint8_t* rdram, recomp_context* ctx, int player, int car_index) {
        Player& pl = players[player];
        Detector& d = pl.det;
        Score& s = pl.score;
        uint32_t car = car_addr(car_index);
        if (car_index < max_cars) {
            car_player[car_index] = player;
        }
        float now = f32(rdram, game_time);
        float dt = f32(rdram, car + car_dt);
        bool wrecked = MEM_B(0, (int32_t)(car + car_wrecked)) != 0;
        bool respawning = MEM_H(0, (int32_t)(car + car_respawn)) != -1;

        // The overlay (func_80391B00) clears a combo's display, and a crash, once it has shown and no trick is going.
        if (!(s.active && (now - s.time < shown_time || s.changes != 0)) && s.changes == 0) {
            s.active = false;
            s.crashed = false;
            s.shown_mult = 1;
            s.shown_landing = 0;
            s.shown_bonus = 0;
            s.shown_points = 0;
            s.shown_count.fill(0);
        }

        if ((d.flags & det_spins) && rush2::wings::gliding(car_index)) {
            s.wings_time += dt;
        }
        if ((d.flags & det_any_but_signs) && (wrecked || respawning)) {
            d.flags = det_reset;
            return;
        }

        Frame f;
        f.now = now;
        f.hit = car_index < max_cars && body_hit[car_index];
        for (int w = 0; w < 4; w++) {
            bool down = (uint16_t)MEM_HU(0, (int32_t)(car + car_wheel_material + (uint32_t)w * 2)) != material_air;
            f.height[w] = f32(rdram, car + car_wheel_height + (uint32_t)w * 4);
            if (down) {
                f.down++;
                (w < 2 ? f.front_down : f.back_down)++;
                ((w & 1) == 0 ? f.left_down : f.right_down)++;
            }
        }
        f.can_trick = f.down < 3 && !wrecked && !respawning;
        for (int i = 0; i < 3; i++) {
            f.step[i] = f32(rdram, car + car_angular_velocity + (uint32_t)i * 4) * dt;
            f.rate[i] = std::fabs(f.step[i]);
        }

        // Which way each axis turns.
        static constexpr uint32_t pos_flag[3] = { det_x_pos, det_y_pos, det_z_pos };
        static constexpr uint32_t neg_flag[3] = { det_x_neg, det_y_neg, det_z_neg };
        for (int i = 0; i < 3; i++) {
            if ((d.flags & pos_flag[i]) && f.step[i] < 0.0f) {
                d.flags = (d.flags & ~pos_flag[i]) | neg_flag[i];
                f.reversed[i] = true;
            } else if ((d.flags & neg_flag[i]) && f.step[i] >= 0.0f) {
                d.flags = (d.flags & ~neg_flag[i]) | pos_flag[i];
                f.reversed[i] = true;
            }
        }

        turn(rdram, ctx, player, f, 2, det_roll, d.roll);
        turn(rdram, ctx, player, f, 1, det_heli, d.spin);
        turn(rdram, ctx, player, f, 0, det_flip, d.flip);

        // A turn over two or three axes.
        if (d.flags & det_multi) {
            if (f.down >= 3) {
                d.flags &= ~det_multi;
            } else if (f.down == 0) {
                for (int i = 0; i < 3; i++) {
                    d.multi[i] += f.rate[i];
                }
            }
            int axes = (d.multi[0] > axis_slip) + (d.multi[1] > axis_slip) + (d.multi[2] > axis_slip);
            if (axes >= 2 && pi <= d.multi[0] + d.multi[1] + d.multi[2]) {
                d.flags &= ~(det_heli | det_roll | det_flip);
                award(rdram, ctx, player, multi, now);
                d.multi[0] = d.multi[1] = d.multi[2] = 0.0f;
            }
        } else if (f.can_trick) {
            d.flags |= det_multi;
            for (int i = 0; i < 3; i++) {
                d.multi[i] = f.rate[i];
            }
        }

        // Air time.
        if (d.flags & det_air) {
            if (f.down > 0 || f.hit) {
                d.air_end = now;
                d.flags &= ~det_air;
                for (float t = d.air_end - d.air_start; t > air_min_time; t -= 1.0f) {
                    award(rdram, ctx, player, air, now);
                }
            }
        } else if (f.down == 0) {
            d.flags |= det_air;
            d.air_end = -1.0f;
            d.air_start = now;
        }

        // Two wheels on one side.
        if (d.flags & det_side) {
            if (f.down >= 3 || f.down < 2) {
                d.side_end = now;
                d.flags &= ~det_side;
                award_timed(rdram, ctx, player, two_wheels_side, d.side_start, d.side_end, now);
            }
        } else if (f.down == 2 && (f.left_down == 2 || f.right_down == 2)) {
            d.flags |= det_side;
            d.side_end = -1.0f;
            d.side_start = now;
        }

        // Front wheels only (func_800C220C).
        const float* h = f.height;
        if (d.flags & det_front) {
            if (f.down >= 3 || f.back_down > 0 || h[0] > 0.75f || h[1] > 0.75f || h[2] < 1.0f || h[3] < 1.0f) {
                d.flags &= ~det_front;
                d.front_end = now;
                award_timed(rdram, ctx, player, two_wheels_front, d.front_start, d.front_end, now);
            }
        } else if ((f.down == 2 && f.front_down == 2) || h[3] > 2.5f || ((h[0] < 0.75f || h[1] < 0.75f) && h[2] > 2.5f)) {
            // As compiled: the back right wheel alone up 2.5 starts it (the cancel above ends it at once if it's wrong).
            d.flags |= det_front;
            d.front_start = now;
            d.front_end = -1.0f;
        }

        // Back wheels only (func_800C2004).
        if (d.flags & det_back) {
            if (f.down >= 3 || f.front_down > 0 || h[2] > 0.75f || h[3] > 0.75f || h[0] < 1.0f || h[1] < 1.0f) {
                d.flags &= ~det_back;
                d.back_end = now;
                award_timed(rdram, ctx, player, two_wheels_back, d.back_start, d.back_end, now);
            }
        } else if ((f.down == 2 && f.back_down == 2) || ((h[2] < 0.75f || h[3] < 0.75f) && h[0] > 2.5f && h[1] > 2.5f)) {
            d.flags |= det_back;
            d.back_start = now;
            d.back_end = -1.0f;
        }

        // Landing: the combo scores a second after the car is back on its wheels.
        if (d.flags & det_tricks) {
            d.settled = -1.0f;
        } else if (f.down >= 3) {
            if (s.changes == 0) {
                s.wings_time = 0.0f;
            }
            if (d.settled == -1.0f && s.changes != 0) {
                d.settled = now;
            }
        }
        if (f.down >= 3 && s.changes != 0 && d.air_end != -1.0f && now - d.air_end > land_delay) {
            d.air_end = -1.0f;
            award(rdram, ctx, player, land, now);
        }
        if (car_index < max_cars) {
            body_hit[car_index] = false;
        }
    }

    // Writes s into Rush 2's panel's sprintf buffer.
    void write_text(uint8_t* rdram, uint32_t buffer, const std::string& s) {
        for (size_t i = 0; i <= s.size(); i++) {
            MEM_B((int32_t)i, (int32_t)buffer) = i < s.size() ? s[i] : 0;
        }
    }
    // ---------------------------------------------------------------------------------------------------------------
    // Rush 2049's stunt HUD (with its scoring and its ROM loaded): Rush 2049's stunt overlay drawn in Rush 2's stunt
    // boxes' places, which Rush 2's own boxes and text leave (rush2_stunt2049_box_visible, rush2_stunt2049_panel_text).
    // Without the ROM, Rush 2's panel shows Rush 2049's scores (rush2_stunt2049_hud).
    // Rush 2049's: mode overlay func_80391B00 (game type 4: the STNT_* icons sliding in, 0x803950C0 + player x 0x2D0,
    // ten 0x48-byte slots) and its text callback func_80392894 (the counts, the combo's points, the "BONUS X n" flash
    // for a new kind of trick, the landing's bonuses, the total).


    constexpr const char* icon_names[trick_count] = {  // 0x80393B40
        "STNT_FLIP_B", "STNT_FLIP_F", "STNT_ROLL_L", "STNT_ROLL_R", "STNT_HELI",
        "STNT_MULTI", "STNT_2WHL_S", "STNT_2WHL_B", "STNT_2WHL_F", "STNT_AIR",
    };
    constexpr int icon_file = 67;
    constexpr int big_font = 10;    // 4BAERO-PLAIN12: one view's text in Rush 2049; here the total
    constexpr int small_font = 12;  // AERO-PLAIN7: split screen's; here the trick box's text
    // Rush 2049's English strings (file 0, entries 179-181) and bonus names (by the score's +0x67).
    constexpr const char* no_wings_text = "NO WINGS BONUS X7";
    constexpr const char* partial_wings_text = "PARTIAL WINGS BONUS X2";
    constexpr const char* bonus_text = "BONUS";
    const char* bonus_name(int kind) {
        static constexpr const char* names[] = {
            "TRIPLE BONUS X10", "QUAD BONUS X20", "RUSH BONUS X40", "SUPER BONUS X60", "EXTREME BONUS X80",
            "SUPER RUSH BONUS X100", "EXTREME RUSH BONUS X500", "PAPYRAY BONUS X1000", "PERFECT SPIN BONUS X15",
            "PERFECT ROLL BONUS X25", "PERFECT FLIP BONUS X25",
        };
        return kind >= 3 && kind <= 13 ? names[kind - 3] : nullptr;
    }
    constexpr uint32_t text_color = 0xE6E6E6FF;  // Rush 2049's text color 1 (0x80118E34 + 8), and its shade
    constexpr uint32_t text_shade = 0x3C3C3CFF;
    constexpr float slide_time = 0.5f;           // a slot's slide (slot +0x44)

    // Decoded on a worker (no ROM reads on the game thread), then copied into RDRAM once.
    struct Assets {
        std::shared_ptr<const rush2::rom2049::Source> rom;
        std::optional<rush2::font2049::Font> fonts[2];         // big, small
        std::array<std::vector<uint16_t>, trick_count> icons;  // RGBA16
        int icon_w = 0, icon_h = 0;
        bool ok = false;
    };
    std::mutex assets_mutex;
    std::shared_ptr<const Assets> assets;  // the last decoded set, for assets->rom
    std::atomic<bool> assets_loading = false;

    // What's in RDRAM, for the set uploaded.
    struct Sheet {
        const rush2::font2049::Font* font = nullptr;
        uint32_t address = 0;
        int w = 0, h = 0, cell_h = 0;
    };
    struct Uploaded {
        std::shared_ptr<const Assets> from;
        Sheet sheets[2];  // big, small
        uint32_t icons[trick_count] = {};
    } uploaded;
    // In spare RDRAM below 16 MB (texture addresses are 24 bits; the recomp heap is above it), between
    // src/rush2049/battle_render.cpp's and the car lighting's (src/controls_menu.cpp's map).
    constexpr uint32_t upload_area = 0x80F18000;
    constexpr uint32_t upload_size = 0x8000;

    void load_assets(std::shared_ptr<const rush2::rom2049::Source> rom) {
        auto out = std::make_shared<Assets>();
        out->rom = rom;
        out->fonts[0] = rush2::font2049::load(*rom, big_font);
        out->fonts[1] = rush2::font2049::load(*rom, small_font);
        std::vector<uint8_t> file;
        bool icons_ok = rom->read_file(icon_file, file);
        for (int i = 0; i < trick_count && icons_ok; i++) {
            std::vector<std::array<uint8_t, 4>> rgba;
            int w = 0, h = 0;
            icons_ok = rush2::rom2049::file_image(file, icon_names[i], rgba, w, h) && w * h <= 2048 &&
                       (i == 0 || (w == out->icon_w && h == out->icon_h));
            out->icon_w = w;
            out->icon_h = h;
            // The icon models (STNT_xxxG1) are 14-sided disks over the texture: its corners don't show.
            float r = std::min(w, h) * 0.5f;
            for (size_t t = 0; t < rgba.size(); t++) {
                const auto& c = rgba[t];
                float dx = (float)(t % w) + 0.5f - w * 0.5f, dy = (float)(t / w) + 0.5f - h * 0.5f;
                bool inside = dx * dx + dy * dy <= r * r && c[3] >= 128;
                out->icons[i].push_back((uint16_t)((c[0] >> 3) << 11 | (c[1] >> 3) << 6 | (c[2] >> 3) << 1 | (inside ? 1 : 0)));
            }
        }
        out->ok = icons_ok;
        for (const auto& f : out->fonts) {
            out->ok = out->ok && f && f->pages.size() == 1 && f->pages[0].w % 16 == 0 && f->pages[0].w * f->pages[0].h <= 8192;
        }
        if (log_tricks) {
            std::fprintf(stderr, "[stunt2049] hud assets: icons %d, font %d %d -> %d\n", (int)icons_ok,
                         out->fonts[0] ? (int)out->fonts[0]->pages.size() : -1,
                         out->fonts[1] ? (int)out->fonts[1]->pages.size() : -1, (int)out->ok);
        }
        std::lock_guard lock{ assets_mutex };
        assets = out;
    }

    // The assets for the loaded ROM, uploaded to RDRAM; null until a worker has decoded them (or if they don't load).
    const Uploaded* ready_assets(uint8_t* rdram) {
        std::shared_ptr<const rush2::rom2049::Source> rom = rush2::wings::get_rom();
        if (rom == nullptr) {
            return nullptr;
        }
        std::shared_ptr<const Assets> a;
        {
            std::lock_guard lock{ assets_mutex };
            a = assets;
        }
        if (a == nullptr || a->rom != rom) {
            if (!assets_loading.exchange(true)) {
                std::thread([rom] { load_assets(rom); assets_loading = false; }).detach();
            }
            return nullptr;
        }
        if (!a->ok) {
            return nullptr;
        }
        if (uploaded.from == a) {
            return &uploaded;
        }
        uint32_t at = upload_area;
        Uploaded u;
        for (int k = 0; k < 2; k++) {
            const rush2::font2049::Font& f = *a->fonts[k];
            const rush2::font2049::Font::Page& p = f.pages[0];
            if (at + (uint32_t)(p.w * p.h / 2) > upload_area + upload_size) return nullptr;
            Sheet& s = u.sheets[k];
            s.font = &f;
            s.address = at;
            s.w = p.w;
            s.h = p.h;
            for (const auto& [c, g] : f.glyphs) {
                s.cell_h = std::max(s.cell_h, g.y1 - g.y0 + 1);
            }
            // I4: two texels a byte, the first in the high nibble.
            for (int t = 0; t + 1 < p.w * p.h; t += 2) {
                MEM_B(t / 2, (int32_t)at) = (int8_t)((p.coverage[t] / 17) << 4 | (p.coverage[t + 1] / 17));
            }
            at += ((uint32_t)(p.w * p.h / 2) + 7) & ~7u;
        }
        for (int i = 0; i < trick_count; i++) {
            if (at + (uint32_t)(a->icons[i].size() * 2) > upload_area + upload_size) return nullptr;
            u.icons[i] = at;
            for (size_t t = 0; t < a->icons[i].size(); t++) {
                MEM_H((int32_t)(t * 2), (int32_t)at) = (int16_t)a->icons[i][t];
            }
            at += ((uint32_t)(a->icons[i].size() * 2) + 7) & ~7u;
        }
        u.from = a;
        uploaded = u;
        return &uploaded;
    }

    bool hud_on() {
        return scoring_2049.load(std::memory_order_relaxed) &&
               uploaded.from != nullptr;
    }

    // A player's icon slots (2049: slot +0 trick, +0x34 alpha, +0x38 position, +0x3C start, +0x40 target, +0x44 timer).
    struct Slot {
        int trick = -1;
        float x = 0, y = 0, start_x = 0, start_y = 0, target_x = 0, target_y = 0, timer = 0;
    };
    struct HudPlayer {
        std::array<Slot, trick_count> slots;  // newest first
        int count = 0;
        int flash = -2;       // 0x80395ED8 + player x 4: frames left of the "BONUS X n" flash
        // The combo's new kinds of trick (Score::new_kinds) already flashed. 2049 counts new_kinds down as it flashes
        // them; here the score is the race's (checked and resynced online) and only the views' players are drawn, so
        // the HUD keeps its own count.
        int kinds_flashed = 0;
        float last_time = 0;  // the clock at the last frame drawn
        bool panel = false;   // Rush 2's stunt panel would show this frame (rush2_stunt2049_box_visible)
        std::string logged;
    };
    std::array<HudPlayer, 4> hud_players;

    // The race player whose score view p's HUD shows: online, this machine's player p's car (the players are the race's
    // cars 0 and up, and this machine's are local_race_player() and on); offline, player p. -1 for none.
    int view_player(int p) {
        if (rush2::online::game::local_race_player() >= 0) {
            int car = rush2::online::game::local_car(p);
            return car >= 0 && car < max_players ? car : -1;
        }
        return p;
    }

    // Sizes are Rush 2049's with one view, in 320 x 240 screen pixels (docs/rush2049_research/stunts.md section 5):
    // an icon model (radius 320 model units, 16 to a unit) scaled 0.005 two units ahead of a 90 degree view is 16 px
    // across, and the text is 4BAERO-PLAIN12. They're laid out from the places of Rush 2's stunt boxes (105 x 40 at
    // scale 1) and drawn at the boxes' scale; at half size (3 or 4 players) the text is AERO-PLAIN7, which Rush 2049
    // uses with more than one view. Trick box: a row of icons with their counts along its bottom, newest first; the
    // combo's points above them; the bonus lines above those (2049 stacks its icons upward too, and has no box behind
    // them). Scores' box: Rush 2049's total box in its middle.
    constexpr float icon_size = 16, icon_bottom = 2, slot_gap = 4, margin = 3;
    // The total's box (func_803925D0, made by func_800B3B4C): "8888888" wide plus 8, a line plus 4 high, a 1 px border.
    constexpr const char* total_box_text = "8888888";
    constexpr uint32_t total_box_fill = 0x00001080, total_box_border = 0x0000C080;

    float text_width(const Sheet& s, const std::string& text) {
        const rush2::font2049::Font& f = *s.font;
        float w = 0;
        for (char c : text) {
            auto g = f.glyphs.find(c);
            w += g == f.glyphs.end() ? (float)f.space : (float)(g->second.x1 - g->second.x0 + 1 + f.gap);
        }
        return std::max(0.0f, w - (float)f.gap);
    }

    // Prints text with its left edge at x and its middle at cy (Rush 2049 prints its stunt text centered on y,
    // 0x80118E24 = 1), `scale` times its size, squeezed to max_w if it's wider.
    void draw_text(uint8_t* rdram, const Sheet& s, const std::string& text, float x, float cy, float scale, float anchor,
                   uint8_t alpha, float max_w = 0) {
        const rush2::font2049::Font& f = *s.font;
        float sx = scale, w = text_width(s, text) * scale;
        if (max_w > 0 && w > max_w) {
            sx *= max_w / w;
        }
        float y0 = cy - s.cell_h * scale * 0.5f;
        uint32_t rgba = (text_color & 0xFFFFFF00u) | alpha;
        for (char c : text) {
            auto g = f.glyphs.find(c);
            if (g == f.glyphs.end()) {
                x += f.space * sx;
                continue;
            }
            const rush2::font2049::Font::Glyph& gl = g->second;
            int gw = gl.x1 - gl.x0 + 1, gh = gl.y1 - gl.y0 + 1;
            rush2::hud::draw_i4_part(rdram, s.address, s.w, s.h, gl.x0, gl.y0, gw, gh, x, y0, x + gw * sx,
                                     y0 + gh * scale, anchor, rgba, text_shade);
            x += (gw + f.gap) * sx;
        }
    }

    void draw_centered(uint8_t* rdram, const Sheet& s, const std::string& text, float cx, float cy, float scale,
                       float anchor, float max_w) {
        float w = max_w > 0 ? std::min(text_width(s, text) * scale, max_w) : text_width(s, text) * scale;
        draw_text(rdram, s, text, cx - w * 0.5f, cy, scale, anchor, 255, max_w);
    }

    void draw_player(uint8_t* rdram, const Uploaded& u, int p, float now) {
        HudPlayer& h = hud_players[p];
        int race_player = view_player(p);
        bool panel = h.panel;
        h.panel = false;
        float dt = std::clamp(now - h.last_time, 0.0f, 0.1f);
        h.last_time = now;
        rush2::hud_layout::StuntPlace place;
        if (race_player < 0 || !panel || !rush2::hud::stunt_place(p, place)) {
            h.count = 0;
            return;
        }
        // The font for the trick box's scale, and how much bigger than its own size it's drawn (box-local units).
        struct Style { const Sheet* sheet; float mul; };
        auto style_for = [&](float scale) {
            return scale < 0.75f ? Style{ &u.sheets[1], 0.5f / std::max(scale, 0.01f) } : Style{ &u.sheets[0], 1.0f };
        };

        // func_80391B00: the counts follow the combo until it lands; the icons show while the combo does. (2049 copies
        // them into the score; the score is the race's, so they're the HUD's own here, as are the points below.)
        const Score& s = players[race_player].score;
        const std::array<int16_t, trick_count> shown_count = s.landed ? s.shown_count : s.count;
        const int32_t shown_points = s.landed ? s.shown_points : s.points;
        bool showing = s.active && (now - s.time < shown_time || s.changes != 0);
        if (!showing && s.changes == 0) {
            h.count = 0;
        }

        const auto& tb = place.box[1];
        Style ts = style_for(tb.scale);
        float line_h = ts.sheet->font->line_height * ts.mul;
        float slot_w = icon_size + 2 + text_width(*ts.sheet, "X 9") * ts.mul + slot_gap;
        float row_y = tb.h - icon_bottom - icon_size;
        float points_cy = row_y - 2 - line_h * 0.5f;

        // A new kind of trick takes the newest slot, sliding in from the bonus lines' place; the older ones slide one
        // place along.
        if (showing) {
            for (int t = 0; t < trick_count; t++) {
                if (shown_count[t] == 0) continue;
                bool has = false;
                for (int i = 0; i < h.count; i++) has = has || h.slots[i].trick == t;
                if (has || h.count >= trick_count) continue;
                for (int i = h.count; i > 0; i--) h.slots[i] = h.slots[i - 1];
                h.count++;
                Slot& n = h.slots[0];
                n = Slot{ t };
                n.x = n.start_x = tb.w * 0.5f - icon_size * 0.5f;
                n.y = n.start_y = points_cy - line_h - icon_size * 0.5f;
                for (int i = 0; i < h.count; i++) {
                    Slot& o = h.slots[i];
                    if (i > 0) {
                        o.start_x = o.x;
                        o.start_y = o.y;
                    }
                    o.target_x = margin + i * slot_w;
                    o.target_y = row_y;
                    o.timer = slide_time;
                }
            }
        }
        for (int i = 0; i < h.count; i++) {
            Slot& o = h.slots[i];
            o.timer = std::max(0.0f, o.timer - dt);
            float k = 1.0f - o.timer / slide_time;
            o.x = o.start_x + (o.target_x - o.start_x) * k;
            o.y = o.start_y + (o.target_y - o.start_y) * k;
        }

        // The bonus lines (func_80392894): with the combo going, "BONUS X n" flashes for ten frames for each new kind
        // of trick; once it has landed, its bonuses.
        std::vector<std::string> lines;
        if (s.new_kinds < h.kinds_flashed) {
            h.kinds_flashed = 0;    // the combo landed: its count started again
        }
        if (s.new_kinds > h.kinds_flashed) {
            if (h.flash == -1) {
                h.flash = -2;
                h.kinds_flashed++;
            } else if (h.flash == -2) {
                h.flash = 10;
            }
            if (h.flash >= 0) {
                lines.push_back(std::string(bonus_text) + " X " + std::to_string(s.shown_mult));
                h.flash--;
            }
        } else {
            h.flash = -2;
        }
        if (s.landed && !s.crashed) {
            if (s.shown_mult >= 2) lines.push_back(std::string(bonus_text) + " X " + std::to_string(s.shown_mult));
            if (const char* name = bonus_name(s.shown_bonus)) lines.push_back(name);
            if (s.shown_landing == 1) lines.push_back(no_wings_text);
            if (s.shown_landing == 2) lines.push_back(partial_wings_text);
        }

        bool trick_drawn = false;
        if (tb.valid && showing) {
            trick_drawn = true;
            float sc = tb.scale;
            auto X = [&](float lx) { return tb.x + lx * sc; };
            auto Y = [&](float ly) { return tb.y + ly * sc; };
            const Sheet& sheet = *ts.sheet;
            float draw_scale = sc * ts.mul;
            for (size_t i = 0; i < lines.size(); i++) {
                float cy = points_cy - (float)(lines.size() - i) * line_h;
                draw_centered(rdram, sheet, lines[i], X(tb.w * 0.5f), Y(cy), draw_scale, tb.anchor, 0);
            }
            draw_centered(rdram, sheet, std::to_string(shown_points), X(tb.w * 0.5f), Y(points_cy), draw_scale,
                          tb.anchor, 0);
            // A crashed combo fades out (func_80391B00, slot +0x34).
            uint8_t alpha = 255;
            if (s.crashed) {
                alpha = (uint8_t)std::clamp((1.0f - (now - s.time) / shown_time) * 255.0f, 0.0f, 255.0f);
            }
            int icon_w = u.from->icon_w, icon_h = u.from->icon_h;
            for (int i = 0; i < h.count; i++) {
                const Slot& o = h.slots[i];
                std::string count = "X " + std::to_string(shown_count[o.trick]);
                // The oldest kinds past the box's right edge aren't shown.
                if (o.timer == 0 && o.x + icon_size + 2 + text_width(sheet, count) * ts.mul > tb.w - margin) {
                    continue;
                }
                rush2::hud::draw_image_part(rdram, u.icons[o.trick], icon_w, icon_h, 0, 0, icon_w, icon_h, X(o.x), Y(o.y),
                                            X(o.x + icon_size), Y(o.y + icon_size), tb.anchor, false, alpha);
                if (shown_count[o.trick] != 0) {
                    draw_text(rdram, sheet, count, X(o.x + icon_size + 2), Y(o.y + icon_size * 0.5f), draw_scale,
                              tb.anchor, alpha);
                }
            }
        }

        // The scores' box: Rush 2049's total box in its middle, unless the trick box is over it.
        const auto& sb = place.box[0];
        if (sb.valid && !(place.swap && trick_drawn)) {
            Style ss = style_for(sb.scale);
            float sc = sb.scale;
            float bw = text_width(*ss.sheet, total_box_text) * ss.mul + 8;
            float bh = ss.sheet->font->line_height * ss.mul + 4;
            float cx = sb.x + sb.w * 0.5f * sc, cy = sb.y + sb.h * 0.5f * sc;
            float x0 = cx - bw * 0.5f * sc, y0 = cy - bh * 0.5f * sc, x1 = cx + bw * 0.5f * sc, y1 = cy + bh * 0.5f * sc;
            float px = std::max(sc, 0.5f);
            rush2::hud::draw_rect(rdram, x0 + px, y0 + px, x1 - px, y1 - px, total_box_fill, sb.anchor);
            rush2::hud::draw_rect(rdram, x0, y0, x1, y0 + px, total_box_border, sb.anchor);
            rush2::hud::draw_rect(rdram, x0, y1 - px, x1, y1, total_box_border, sb.anchor);
            rush2::hud::draw_rect(rdram, x0, y0 + px, x0 + px, y1 - px, total_box_border, sb.anchor);
            rush2::hud::draw_rect(rdram, x1 - px, y0 + px, x1, y1 - px, total_box_border, sb.anchor);
            draw_centered(rdram, *ss.sheet, std::to_string(s.total), cx, cy, sc * ss.mul, sb.anchor, 0);
        }

        if (log_tricks) {
            std::string shown = "total " + std::to_string(s.total);
            if (trick_drawn) {
                shown += " | points " + std::to_string(shown_points) + " |";
                for (int i = 0; i < h.count; i++) {
                    shown += std::string(" ") + trick_text[h.slots[i].trick] + " X " +
                             std::to_string(shown_count[h.slots[i].trick]);
                }
                for (const std::string& l : lines) shown += " | " + l;
            }
            if (shown != h.logged) {
                h.logged = shown;
                std::fprintf(stderr, "[stunt2049] p%d hud: %s\n", p, shown.c_str());
            }
        }
    }
}


void rush2::stunt2049::set_scoring(Scoring scoring) {
    scoring_2049.store(scoring == Scoring::Rush2049, std::memory_order_relaxed);
}

bool rush2::stunt2049::enabled() {
    return scoring_2049.load(std::memory_order_relaxed);
}

void rush2::stunt2049::reset() {
    for (Player& p : players) {
        p = Player{};
        reset_detector(p.det);
    }
    body_hit.fill(false);
    car_player.fill(-1);
    hud_players = {};
}

void rush2::stunt2049::draw_hud(uint8_t* rdram) {
    if (!scoring_2049.load(std::memory_order_relaxed)) {
        return;
    }
    const Uploaded* u = ready_assets(rdram);
    if (u == nullptr) {
        return;
    }
    float now = f32(rdram, game_time);
    for (int p = 0; p < (int)hud_players.size(); p++) {
        draw_player(rdram, *u, p, now);
    }
}

// race_load_track_800A5110 (0x800A5114): a race is being set up.
extern "C" void rush2_stunt2049_race_setup(uint8_t* rdram, recomp_context* ctx) {
    rush2::stunt2049::reset();
}

// car_stunt_scoring_update_80065588 at its start, $a0 = the car, $a1 = the player. Returns true to skip Rush 2's.
extern "C" int rush2_stunt2049_update(uint8_t* rdram, recomp_context* ctx) {
    if (!rush2::stunt2049::enabled()) {
        return 0;
    }
    int car = (int16_t)ctx->r4;
    int player = (int32_t)ctx->r5;
    if (player >= 0 && player < max_players && car >= 0) {
        update(rdram, ctx, player, car);
    }
    return 1;
}

// A wreck drops the combo (Rush 2049 calls func_800C3578 from its wreck trigger func_800C54F0, after the 5 s guard, and
// from its crash in func_800E6AF8; Rush 2's scoring isn't run for a wrecked car). Called with the car from
// car_wreck_trigger_8006E1A8 at 0x8006E210 (after its 5 s guard, $t6 = the car) and race_wreck_respawn_wait_8008E40C at
// 0x8008E4C0 (a car's wreck begins, $t0 = the car).
extern "C" void rush2_stunt2049_wreck(uint8_t* rdram, recomp_context* ctx, int car) {
    if (!rush2::stunt2049::enabled() || car < 0 || car >= max_cars || car_player[car] < 0) {
        return;
    }
    crash(rdram, ctx, car_player[car], f32(rdram, game_time));
}

// car_wall_collision_query_8006FC94 at 0x8006FF60: a body point is pushed out of the world. $s1 = the car.
extern "C" void rush2_stunt2049_body_hit(uint8_t* rdram, recomp_context* ctx) {
    uint32_t car = (uint32_t)ctx->r17;
    if (car >= cars && (car - cars) % car_size == 0 && (car - cars) / car_size < (uint32_t)max_cars) {
        body_hit[(car - cars) / car_size] = true;
    }
}

// hud_widget_text_cb_player_stats_800B7654, $s6 = the player, $s5 = its sprintf buffer. step 0 at 0x800B7838 (before
// the trick name is measured and printed: points its name table slot at the combo's name), 1 at 0x800B78C0 (puts the
// slot back), 2 at 0x800B78F4 (the points line), 3 at 0x800B7974 (BEST's value), 4 at 0x800B79F8 (TOTAL's).
extern "C" void rush2_stunt2049_hud(uint8_t* rdram, recomp_context* ctx, int step) {
    if (step == 1) {
        int player = (int32_t)ctx->r22;
        if (log_tricks && player >= 0 && player < max_players) {
            // The name just printed, from the slot the print read.
            uint32_t lang = (uint32_t)MEM_W(0, (int32_t)language);
            uint32_t idx = (uint8_t)MEM_BU(player, (int32_t)rush2::players4::moved(hud_trick_name));
            printed_name[player].clear();
            for (uint32_t at = (uint32_t)MEM_W(0, (int32_t)(trick_names + lang * 0x14 + idx * 4));
                 MEM_BU(0, (int32_t)at) != 0 && printed_name[player].size() < 32; at++) {
                printed_name[player] += (char)MEM_BU(0, (int32_t)at);
            }
        }
        if (patched_slot != 0) {
            MEM_W(0, (int32_t)patched_slot) = (int32_t)saved_name;
            patched_slot = 0;
        }
        return;
    }
    int player = (int32_t)ctx->r22;
    // The panel's player is a view's (online the presentation puts this machine's players first: stunt_rotate in
    // src/online/online_game.cpp); the score is its race player's.
    int scorer = player >= 0 && player < max_players ? view_player(player) : -1;
    if (!rush2::stunt2049::enabled() || scorer < 0) {
        return;
    }
    const Score& s = players[scorer].score;
    uint32_t buffer = (uint32_t)ctx->r21;
    switch (step) {
        case 0: {
            if (s.shown_name.empty()) {
                break;
            }
            if (text_scratch == 0) {
                text_scratch = (uint32_t)((uint8_t*)recomp::alloc(rdram, text_slot * max_players) - rdram) + 0x80000000u;
            }
            uint32_t text = text_scratch + (uint32_t)player * text_slot;
            write_text(rdram, text, s.shown_name.substr(0, text_slot - 1));
            uint32_t lang = (uint32_t)MEM_W(0, (int32_t)language);
            uint32_t idx = (uint8_t)MEM_BU(player, (int32_t)rush2::players4::moved(hud_trick_name));
            patched_slot = trick_names + lang * 0x14 + idx * 4;
            saved_name = (uint32_t)MEM_W(0, (int32_t)patched_slot);
            MEM_W(0, (int32_t)patched_slot) = (int32_t)text;
            break;
        }
        case 2: {
            std::string line = std::to_string(s.shown_base);
            if (s.panel_mult >= 2) {
                line += " X" + std::to_string(s.panel_mult);
            }
            write_text(rdram, buffer, line);
            if (log_tricks) {
                static std::string last[max_players];
                std::string shown = printed_name[player] + " / " + line;
                if (shown != last[player]) {
                    last[player] = shown;
                    std::fprintf(stderr, "[stunt2049] p%d panel: %s\n", player, shown.c_str());
                }
            }
            break;
        }
        case 3:
            write_text(rdram, buffer, std::to_string(s.best));
            break;
        case 4:
            write_text(rdram, buffer, std::to_string(s.total));
            break;
    }
}

// hud_stunt_box_visible_80060E34 at 0x80060E98, its first hide test ($a1 = 1 hides the box; $a0 = the widget, +0x2C =
// player << 4 | box). With the Rush 2049 Stunt HUD the box is hidden, and whether Rush 2's panel would show (the
// stunt arena or Stunts on All Tracks, a player in the race) is kept for draw_hud.
extern "C" void rush2_stunt2049_box_visible(uint8_t* rdram, recomp_context* ctx) {
    if (!hud_on()) {
        return;
    }
    int player = (int)(((uint32_t)MEM_W(0x2C, (int32_t)ctx->r4) >> 4) & 0xF);
    constexpr uint32_t race_player_count = 0x8010C3E2;
    if (ctx->r5 == 0 && player < 4 && player < MEM_H(0, (int32_t)race_player_count)) {
        hud_players[player].panel = true;
    }
    ctx->r5 = 1;
}

// hud_widget_text_cb_player_stats_800B7654 at 0x800B76CC ($a1 = 1 hides the panel's text): Rush 2's stunt text isn't
// printed under the Rush 2049 Stunt HUD.
extern "C" void rush2_stunt2049_panel_text(uint8_t* rdram, recomp_context* ctx) {
    if (hud_on()) {
        ctx->r5 = 1;
    }
}

// Online play's checks and resyncs (include/online_sync.h): each player's trick detector and combo, which score the
// stunt points.
void rush2::stunt2049::online_sync(rush2::online::sync::Walker& w) {
    namespace s = rush2::online::sync;
    for (Player& p : players) {
        Detector& d = p.det;
        s::field(w, d.flags);
        s::field(w, d.roll);
        s::field(w, d.flip);
        s::field(w, d.spin);
        s::field(w, d.multi);
        s::field(w, d.air_start);
        s::field(w, d.air_end);
        s::field(w, d.side_start);
        s::field(w, d.side_end);
        s::field(w, d.front_start);
        s::field(w, d.front_end);
        s::field(w, d.back_start);
        s::field(w, d.back_end);
        s::field(w, d.settled);
        Score& c = p.score;
        s::field(w, c.active);
        s::field(w, c.crashed);
        s::field(w, c.time);
        s::field(w, c.best);
        s::field(w, c.points);
        s::field(w, c.shown_points);
        s::field(w, c.total);
        s::field(w, c.changes);
        s::field(w, c.last);
        s::field(w, c.count);
        s::field(w, c.shown_count);
        s::field(w, c.all_count);
        s::field(w, c.wings_time);
        s::field(w, c.landing);
        s::field(w, c.shown_landing);
        s::field(w, c.bonus);
        s::field(w, c.shown_bonus);
        s::field(w, c.mult);
        s::field(w, c.shown_mult);
        s::field(w, c.new_kinds);
        s::field(w, c.landed);
        s::field(w, c.shown_name);
        s::field(w, c.shown_base);
        s::field(w, c.panel_mult);
    }
    s::field(w, body_hit);
    s::field(w, car_player);
}
