// The car select's ENGINE row revs the chosen engine, as its HORN row plays the chosen horn.
//
// Rush 2's engine isn't a one-shot sound but a voice per local player slot (0-1) that the audio thread keeps
// retuning. Race setup starts it (func_8009E664 -> func_8008C6E8 -> func_8008C61C(engine, slot)): func_8008C61C sets
// 0x800D56F8[slot] = engine + 10 * slot + 1 and sends command 0x1FFE + slot, which starts the engine's loop sounds
// (up to two per engine, table 0x800BD1FC). Every audio frame func_80062CC8 then sets their pitch from the slot's rpm
// (0x800FF3C0[slot], over a per-sound base of 3000-6000) and their volume from the rpm and the load
// (0x8010BCD0[slot]); in a race func_80064F70 writes those from the car each frame. func_8008C49C stops every slot's
// engine (command 0x8000 | sound for each of its sounds) and clears 0x800D56F8.
//
// Here a change on the ENGINE row (or its reset, like the horn) starts that engine on the player's slot and the main
// loop (func_800AE670, once per frame) revs it up and back down for 1.6 s, then stops it. The rev also stops as soon
// as the car select stops running, and before race setup starts the race's engines. A 2049 car revs Rush 2049's
// engine for its ENGINE level instead (src/engine2049.cpp).

#include <chrono>
#include <cstring>

#include "recomp.h"

#include "car2049.h"
#include "engine2049.h"
#include "rush2_hooks.h"

extern "C" void func_8008C61C(uint8_t* rdram, recomp_context* ctx); // Starts engine a0 on slot a1.
extern "C" void func_8008C49C(uint8_t* rdram, recomp_context* ctx); // Stops both slots' engines.

namespace {
    constexpr uint32_t engine_ids = 0x800D56F8;   // s32 [2]: engine + 10 * slot + 1, 0 = none
    constexpr uint32_t engine_rpm = 0x800FF3C0;   // f32 [2]
    constexpr uint32_t engine_load = 0x8010BCD0;  // f32 [2]
    constexpr uint32_t engine_boost = 0x8010C0B8; // f32 [2]: the turbo whine's level (sound 0x0C)
    constexpr uint32_t engine_ramp = 0x8010C0C8;  // s32 [2]
    constexpr int engines = 10;

    using Clock = std::chrono::steady_clock;

    struct Rev {
        bool active = false;
        bool rush2049 = false; // a 2049 car's engine (src/engine2049.cpp)
        Clock::time_point start;
    };
    Rev revs[2];
    bool car_select_ran = false;

    // The rev: idle, throttle up to 6500 rpm, hold, lift off back to idle, stop.
    constexpr float idle_rpm = 1500.0f, peak_rpm = 6500.0f;
    constexpr float idle_load = 100.0f, peak_load = 240.0f;
    constexpr float t_rise = 0.15f, t_peak = 0.6f, t_fall = 0.75f, t_idle = 1.35f, t_end = 1.6f;

    void put_f32(uint8_t* rdram, uint32_t addr, float value) {
        int32_t bits;
        std::memcpy(&bits, &value, sizeof(bits));
        MEM_W(0, (int32_t)addr) = bits;
    }

    void call(uint8_t* rdram, recomp_context* ctx, void (*func)(uint8_t*, recomp_context*), int32_t a0 = 0,
              int32_t a1 = 0) {
        recomp_context saved = *ctx;
        ctx->r4 = (uint64_t)(int64_t)a0;
        ctx->r5 = (uint64_t)(int64_t)a1;
        func(rdram, ctx);
        *ctx = saved;
    }

    void set_rev(uint8_t* rdram, int slot, float rpm, float load) {
        if (revs[slot].rush2049) {
            rush2::engine2049::preview_update(rdram, slot, rpm, (load - idle_load) / (peak_load - idle_load));
            return;
        }
        put_f32(rdram, engine_rpm + slot * 4, rpm);
        put_f32(rdram, engine_load + slot * 4, load);
    }

    // Stops one slot's engine: func_8008C49C stops only the slots with an engine set, so the other is hidden from it.
    void stop(uint8_t* rdram, recomp_context* ctx, int slot) {
        revs[slot].active = false;
        if (revs[slot].rush2049) {
            rush2::engine2049::preview_stop(slot);
            return;
        }
        uint32_t other = engine_ids + (slot ^ 1) * 4;
        int32_t kept = MEM_W(0, (int32_t)other);
        MEM_W(0, (int32_t)other) = 0;
        call(rdram, ctx, func_8008C49C);
        MEM_W(0, (int32_t)other) = kept;
    }

    void stop_all(uint8_t* rdram, recomp_context* ctx) {
        for (int slot = 0; slot < 2; slot++) {
            if (revs[slot].active) {
                stop(rdram, ctx, slot);
            }
        }
    }

    float lerp(float a, float b, float t) {
        return a + (b - a) * t;
    }
}

namespace {
    // The rev's rpm at t seconds after its start.
    float rev_rpm(float t) {
        if (t < t_rise || t >= t_idle) return idle_rpm;
        if (t < t_peak) return lerp(idle_rpm, peak_rpm, (t - t_rise) / (t_peak - t_rise));
        if (t < t_fall) return peak_rpm;
        return lerp(peak_rpm, idle_rpm, (t - t_fall) / (t_idle - t_fall));
    }
}

// Car select, func_803B9478, once per frame.
extern "C" void rush2_engine_preview_car_select(uint8_t*, recomp_context*) {
    car_select_ran = true;
}

void rush2::engine2049::shop_frame() {
    car_select_ran = true;
}

void rush2::engine2049::shop_rev(uint8_t* rdram, recomp_context* ctx, int level) {
    if (revs[0].active) {
        stop(rdram, ctx, 0);
    }
    preview_start(rdram, 0, level);
    revs[0].rush2049 = true;
    set_rev(rdram, 0, idle_rpm, idle_load);
    revs[0].active = true;
    revs[0].start = Clock::now();
}

float rush2::engine2049::shop_rev_amount() {
    if (!revs[0].active) {
        return 0.0f;
    }
    float t = std::chrono::duration<float>(Clock::now() - revs[0].start).count();
    return (rev_rpm(t) - idle_rpm) / (peak_rpm - idle_rpm);
}

// Car select, func_803B9478 at 0x803B9EBC: the ENGINE row's new value is set (player $s7, type $a3, value $v0), right
// where the HORN row plays its horn.
extern "C" void rush2_engine_preview_start(uint8_t* rdram, recomp_context* ctx) {
    int slot = (int)ctx->r23;
    if (slot < 0 || slot > 1) {
        return;
    }
    int type = (int)(int8_t)ctx->r7;
    int engine = rush2::car2049::engine_sound(rdram, type, (int)(int8_t)ctx->r2);
    if (engine < 0 || engine >= engines) {
        return;
    }
    if (revs[slot].active) {
        stop(rdram, ctx, slot);
    }
    if (type >= rush2::car2049::first_type && type < rush2::car2049::types) {
        rush2::engine2049::preview_start(rdram, slot, (int)(int8_t)ctx->r2);
        revs[slot].rush2049 = true;
        set_rev(rdram, slot, idle_rpm, idle_load);
        revs[slot].active = true;
        revs[slot].start = Clock::now();
        return;
    }
    revs[slot].rush2049 = false;
    set_rev(rdram, slot, idle_rpm, idle_load);
    put_f32(rdram, engine_boost + slot * 4, 0.0f);
    MEM_W(0, (int32_t)(engine_ramp + slot * 4)) = 0;
    rush2::engine2049::set_preview_call(true);
    call(rdram, ctx, func_8008C61C, engine, slot);
    rush2::engine2049::set_preview_call(false);
    revs[slot].active = true;
    revs[slot].start = Clock::now();
}

// func_800AE670 (the main loop's game state step), once per frame.
extern "C" void rush2_engine_preview_frame(uint8_t* rdram, recomp_context* ctx) {
    bool in_car_select = car_select_ran;
    car_select_ran = false;
    if (!in_car_select) {
        stop_all(rdram, ctx);
        return;
    }
    for (int slot = 0; slot < 2; slot++) {
        Rev& rev = revs[slot];
        if (!rev.active) {
            continue;
        }
        float t = std::chrono::duration<float>(Clock::now() - rev.start).count();
        if (t >= t_end) {
            stop(rdram, ctx, slot);
        }
        else if (t < t_rise) {
            set_rev(rdram, slot, idle_rpm, idle_load);
        }
        else if (t < t_peak) {
            float k = (t - t_rise) / (t_peak - t_rise);
            set_rev(rdram, slot, lerp(idle_rpm, peak_rpm, k), lerp(idle_load, peak_load, k));
        }
        else if (t < t_fall) {
            set_rev(rdram, slot, peak_rpm, peak_load);
        }
        else if (t < t_idle) {
            float k = (t - t_fall) / (t_idle - t_fall);
            set_rev(rdram, slot, lerp(peak_rpm, idle_rpm, k), idle_load);
        }
        else {
            set_rev(rdram, slot, idle_rpm, idle_load);
        }
    }
}

// func_8009E6DC (race setup), before it starts the race's engines.
extern "C" void rush2_engine_preview_stop(uint8_t* rdram, recomp_context* ctx) {
    stop_all(rdram, ctx);
}
