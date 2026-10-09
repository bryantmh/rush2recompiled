// Rush 2049's engine sounds for the 2049 cars.
//
// Rush 2049 (func_800D5E64 starts, func_800E0050 updates, every other frame) gives each car up to two looping engine
// layers picked by its ENGINE setting, not by the car: table 0x8010FD80 + ENGINE * 0x40 (rush2::car2049::EngineLayer).
// From the rpm (car +0x7D0 = engine rad/s x 9.549 x 0.9) each layer gets
//   pitch  = 1 + (rpm - base) / span, clamped to 0-2,
//   volume = v0 below rpm point 0, v0 -> v1 -> v2 linearly up to point 2, v2 above,
// times the load factor 0.85 + (load + 200) / 900 x 0.15 (car +0x404, the engine torque). The local player's own car
// plays at that volume x (0.8 - 0.05 x players), without position; other cars are 3D emitters at 0.75 of it. Outside
// races (0x801174B4 without 0x400000) other cars' rpm is held at 890, and they also get a random sound 98-100.
//
// Rush 2 plays an engine only per local player slot (src/engine_preview.cpp has its details: func_8008C61C starts
// engine a0 on slot a1 at race start and after a pause, func_8008C49C stops both, and func_80064F70 hands the slot the
// car's rpm and load every frame). A slot whose car is a 2049 car gets 2049's engine in place of Rush 2's: its start is
// skipped, and each func_80064F70 call updates the 2049 layers from the car's +0x7F0 (Rush 2 keeps 2049's rpm x 0.9
// there) and +0x3F4 (the torque, 2049's +0x404), as 2049 plays the local player's car. Rush 2's computer cars have no
// engine sound; src/car_engines.cpp gives the other cars theirs (an option).

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <mutex>

#include "recomp.h"
#include "rush2_hooks.h"
#include "audio2049.h"
#include "car2049.h"
#include "car_engines.h"
#include "engine2049.h"
#include "track2049.h"

namespace audio = rush2::audio2049;

extern "C" void osSendMesg_recomp(uint8_t* rdram, recomp_context* ctx); // 0x80006FD0

namespace {
    constexpr uint32_t players = 0x800C2140;       // Per local player slot, 0x28 bytes: +0 car index
    constexpr uint32_t player_size = 0x28;
    constexpr uint32_t physics_cars = 0x800F5470;  // + car index * 0x81C
    constexpr uint32_t physics_car_size = 0x81C;
    constexpr uint32_t car_rpm = 0x7F0;            // s16: engine rad/s x 9.549 x 0.9 (func_80069254)
    constexpr uint32_t car_load = 0x3F4;           // f32: engine torque
    constexpr uint32_t car_states = 0x801124A0;    // + car index * 0x354; +0x343 > 0: engine off (func_80064F70)
    constexpr uint32_t car_state_size = 0x354;
    constexpr uint32_t num_players = 0x8010C3E2;   // s16
    constexpr uint32_t engine_lock = 0x800D3E48;   // OSMesgQueue: func_8008C61C's lock

    struct Engine {
        bool active = false;
        int level = -1;
        int handles[2] = { -1, -1 };
    };
    std::mutex engines_mutex;
    Engine race[2];
    Engine preview[2];
    std::atomic_bool preview_call = false;

    void stop(Engine& e) {
        for (int& h : e.handles) {
            if (h >= 0) audio::sfx_stop(h);
            h = -1;
        }
    }

    // func_800E0050 for one car at `rpm` (2049's +0x7D0) and `load` (+0x404), at `scale` of the layers' volume.
    void update(uint8_t* rdram, Engine& e, int level, float rpm, float load, float scale) {
        if (level != e.level) {
            stop(e);
            e.level = level;
        }
        rush2::engine2049::LayerMix mix[2];
        if (!rush2::track2049::music_ready() || !rush2::engine2049::layer_mix(level, rpm, load, mix)) {
            return;
        }
        for (int l = 0; l < 2; l++) {
            if (mix[l].sound < 0) continue;
            float volume = std::clamp(mix[l].volume * scale, 0.0f, 1.0f);
            int& h = e.handles[l];
            if (h < 0 || !audio::sfx_active(h)) {
                h = audio::sfx_start(mix[l].sound, volume, 0.0f, mix[l].pitch);
                fprintf(stderr, "[2049] Engine level %d layer %d: sound 0x%02X (handle %d)\n", level + 1, l, mix[l].sound, h);
            }
            else {
                audio::sfx_update(h, volume, 0.0f, mix[l].pitch);
            }
        }
        rush2::track2049::effects_running(rdram);
    }

    // 2049's volume for the local player's own car.
    float player_scale(uint8_t* rdram) {
        int n = std::clamp<int>((int16_t)MEM_H(0, (int32_t)num_players), 1, 4);
        return 0.8f - 0.05f * float(n);
    }

    uint32_t slot_car(uint8_t* rdram, int slot, int& index) {
        index = MEM_BU(0, (int32_t)(players + slot * player_size));
        return physics_cars + uint32_t(index) * physics_car_size;
    }
}

// func_8008C61C at 0x8008C684, once it holds its lock (message queue 0x800D3E48, which its first call creates; slot
// at sp + 0x36): a slot driving a 2049 car doesn't start Rush 2's engine. The function is left here, releasing the lock
// and its stack frame (0x30) as its end does.
extern "C" int rush2_engine49_start(uint8_t* rdram, recomp_context* ctx) {
    uint32_t sp = (uint32_t)ctx->r29;
    int slot = (int16_t)MEM_H(0, (int32_t)(sp + 0x36));
    if (preview_call || slot < 0 || slot > 1) {
        return 0;
    }
    int index;
    uint32_t car = slot_car(rdram, slot, index);
    {
        std::lock_guard lock{ engines_mutex };
        if (index >= 8 || rush2::car2049::engine_level(rdram, car) < 0) {
            race[slot].active = false;
            stop(race[slot]);
            return 0;
        }
        race[slot].active = true;
    }
    recomp_context saved = *ctx;
    ctx->r4 = (uint64_t)(int64_t)(int32_t)engine_lock;
    ctx->r5 = 0;
    ctx->r6 = 0;
    osSendMesg_recomp(rdram, ctx);
    *ctx = saved;
    ctx->r29 = (uint64_t)(int64_t)(int32_t)(sp + 0x30);
    return 1;
}

// func_8008C49C entry: Rush 2 stops the engines (pause, race end); a pause's end starts them again.
extern "C" void rush2_engine49_stop(uint8_t* rdram, recomp_context* ctx) {
    rush2::car_engines::stop();
    std::lock_guard lock{ engines_mutex };
    for (Engine& e : race) {
        stop(e);
    }
}

// func_80064F70 entry ($a0 rpm, $a1 load, $a2 slot, both 0 with the engine off): the slot's car, every frame.
extern "C" void rush2_engine49_tick(uint8_t* rdram, recomp_context* ctx) {
    int slot = (int16_t)ctx->r6;
    if (slot < 0 || slot > 1) {
        return;
    }
    // The other cars' engines (src/car_engines.cpp) follow the first local player's.
    if (slot == 0) {
        rush2::car_engines::update(rdram);
    }
    std::lock_guard lock{ engines_mutex };
    Engine& e = race[slot];
    if (!e.active) {
        return;
    }
    int index;
    uint32_t car = slot_car(rdram, slot, index);
    int level = index < 8 ? rush2::car2049::engine_level(rdram, car) : -1;
    if (level < 0) {
        stop(e);
        return;
    }
    bool off = ((ctx->r4 & 0x7FFF) == 0 && (ctx->r5 & 0x7FFF) == 0) ||
               (int8_t)MEM_B(0, (int32_t)(car_states + index * car_state_size + 0x343)) > 0;
    float rpm = 0.0f, load = 0.0f;
    if (!off) {
        rpm = (float)(int16_t)MEM_H(0, (int32_t)(car + car_rpm));
        uint32_t bits = (uint32_t)MEM_W(0, (int32_t)(car + car_load));
        std::memcpy(&load, &bits, 4);
    }
    update(rdram, e, level, rpm, load, player_scale(rdram));
}

bool rush2::engine2049::layer_mix(int level, float rpm, float load, LayerMix out[2]) {
    rush2::car2049::EngineLayer layers[2];
    if (!rush2::car2049::engine_layers(level, layers)) {
        return false;
    }
    rpm = std::fabs(rpm);
    float load_factor = 0.85f + (load + 200.0f) / 900.0f * 0.15f;
    for (int l = 0; l < 2; l++) {
        const rush2::car2049::EngineLayer& layer = layers[l];
        out[l] = LayerMix{};
        if (layer.sound < 0 || layer.span <= 0.0f) continue;
        const float* p = layer.rpm;
        const float* v = layer.volume;
        float volume = rpm < p[0] ? v[0]
                     : rpm < p[1] ? v[0] + (v[1] - v[0]) * (rpm - p[0]) / (p[1] - p[0])
                     : rpm < p[2] ? v[1] + (v[2] - v[1]) * (rpm - p[1]) / (p[2] - p[1])
                     : v[2];
        out[l].sound = layer.sound;
        out[l].pitch = std::clamp(1.0f + (rpm - layer.base) / layer.span, 0.0f, 2.0f);
        out[l].volume = std::max(0.0f, volume * load_factor);
    }
    return true;
}

void rush2::engine2049::preview_start(uint8_t* rdram, int slot, int level) {
    if (slot < 0 || slot > 1) return;
    std::lock_guard lock{ engines_mutex };
    stop(preview[slot]);
    preview[slot].active = true;
    preview[slot].level = level;
}

void rush2::engine2049::preview_update(uint8_t* rdram, int slot, float rpm, float throttle) {
    if (slot < 0 || slot > 1) return;
    std::lock_guard lock{ engines_mutex };
    Engine& e = preview[slot];
    if (!e.active) return;
    // Rush 2049 keeps rpm x 0.9; its torque runs from about -200 (lifting off) to 700.
    update(rdram, e, e.level, rpm * 0.9f, -200.0f + 900.0f * std::clamp(throttle, 0.0f, 1.0f), 0.75f);
}

void rush2::engine2049::preview_stop(int slot) {
    if (slot < 0 || slot > 1) return;
    std::lock_guard lock{ engines_mutex };
    stop(preview[slot]);
    preview[slot].active = false;
}

void rush2::engine2049::set_preview_call(bool on) {
    preview_call = on;
}
