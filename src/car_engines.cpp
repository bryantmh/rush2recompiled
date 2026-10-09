// Engine sounds of the cars the local players don't drive, and of players 3 and 4's cars.
//
// Rush 2 plays an engine only for each local player's own car (src/engine_preview.cpp: two slots, func_80062CC4
// retunes them every audio frame); computer cars are silent. Players 3 and 4 (src/players4.cpp) have no slot, so their
// cars' engines are played here at a player's own volume, whether or not the other cars' engines are on. Rush 2049 plays every other car's engine as a 3D emitter
// (src/rush2049/engine2049.cpp, docs/rush2049_research/audio.md §7), so the same is done here for both games' cars, heard from
// the local players' cars with Rush 2049's emitter law (audio2049::emitter_mix: range 400, other cars at 0.75 of their
// engine volume). Each car keeps its own game's engine:
// - A 2049 car plays Rush 2049's engine for its ENGINE level through the 2049 sound system (rush2::audio2049).
// - A Rush 2 car plays Rush 2's engine for its default engine sound with Rush 2's law. Its samples are read from the
//   game's sound effect bank (the ALInstrument at 0x800D2478, as func_80062264 starts them) and decoded from the ROM
//   here, and mixed into the game's output, since Rush 2's own player has voices for only the two local engines.
//
// Rush 2's engine law (func_80062640 / func_80062598 / func_800624E4), from the slot's rpm r = |car +0x7F0| x
// [0x800E7BA4] and load l = car +0x3F4 x [0x800E7BAC] (both as u16 & 0x7FFF, func_800650DC / func_80064F70):
//   layer sound   = 0x800BD1FC [engine * 2 + layer] (-1 none; 0xC is the turbo whine, left out here)
//   pitch         = min(r / 0x800BD15C [engine * 2 + layer], 2)
//   volume (s16)  = (int)((int)(A[layer][min(r / 1000, 10)] x B[layer][clamp((l + 80) x 0.025, 0, 12)] x 2000 + 24000)
//                   x engine volume (0x800D577A) / 40), A = 0x800BD338 (2 x 11), B = 0x800BD29C (2 x 13).
// libaudio gives a voice volume / 32767 x the sound's sample volume / 127 x its envelope's sustain / 127, and an
// equal-power pan.

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <span>
#include <vector>

#include "recomp.h"
#include "librecomp/game.hpp"
#include "audio2049.h"
#include "car2049.h"
#include "car_engines.h"
#include "engine2049.h"
#include "track2049.h"

namespace audio = rush2::audio2049;

namespace {
    constexpr int max_cars = 8;
    constexpr int max_players = 4;
    constexpr uint32_t physics_cars = 0x800F5470;  // + car index * 0x81C
    constexpr uint32_t physics_car_size = 0x81C;
    constexpr uint32_t car_position = 0x224;       // f32 [3]
    constexpr uint32_t car_rpm = 0x7F0;            // s16
    constexpr uint32_t car_load = 0x3F4;           // f32
    constexpr uint32_t car_engine_off = 0x648;     // s8: nonzero = no engine (func_800650DC)
    constexpr uint32_t car_active = 0x7E4;         // s16: 0 = not racing (func_80099D88)
    constexpr uint32_t car_driver = 0x7E8;         // s8: 2 = a player
    constexpr uint32_t car_type = 0x7EA;           // u8
    constexpr uint32_t car_states = 0x801124A0;    // + car index * 0x354; +0x343 > 0: engine off
    constexpr uint32_t car_state_size = 0x354;
    constexpr uint32_t players = 0x800C2140;       // Per local player slot, 0x28 bytes: +0 car index
    constexpr uint32_t player_size = 0x28;
    constexpr uint32_t num_players = 0x8010C3E2;   // s16
    constexpr uint32_t cameras = 0x800E79D0;       // Per view, 0x40 bytes: rows +0x0 right, +0xC up, +0x18 back
    constexpr uint32_t camera_size = 0x40;
    constexpr uint32_t rpm_scale = 0x800E7BA4;     // f32 (set at runtime)
    constexpr uint32_t load_scale = 0x800E7BAC;    // f32 (set at runtime)
    constexpr uint32_t engines_muted = 0x800E7BCE; // u8: nonzero = every engine at 0 rpm (func_800650DC)
    constexpr uint32_t engine_volume = 0x800D577A; // s8: 0-40
    constexpr uint32_t sound_bank = 0x800D2478;    // ALInstrument* of the sound effects
    constexpr uint32_t engine_sounds = 0x800BD1FC; // s32 [10][2]
    constexpr uint32_t engine_bases = 0x800BD15C;  // f32 [10][2]
    constexpr uint32_t rpm_table = 0x800BD338;     // f32 [2][11]
    constexpr uint32_t load_table = 0x800BD29C;    // f32 [2][13]
    constexpr uint32_t default_engines = 0x80200C88; // s8 [36]: each type's engine sound (src/rush2049/car2049.cpp)
    constexpr int turbo_sound = 0xC;
    constexpr float range = 400.0f;
    // Rush 2049 uses 0.75 (func_800E0050), but with up to seven cars around that drowned out the player's own.
    constexpr float other_car_volume = 0.4f;

    std::atomic_bool option = true;

    float f32_at(uint8_t* rdram, uint32_t addr) {
        uint32_t bits = (uint32_t)MEM_W(0, (int32_t)addr);
        float f;
        memcpy(&f, &bits, 4);
        return f;
    }

    // A Rush 2 sound decoded to PCM.
    struct Sample {
        std::vector<int16_t> pcm;
        uint32_t loop_start = 0, loop_end = 0; // loop_end 0: no loop
        float level = 1.0f;                     // sample volume x envelope sustain
        float rate = 1.0f;                      // the key map's detune
    };

    // The standard N64 VADPCM decoder (9-byte frames of 16 samples, order-2 predictors), as the RSP decodes.
    bool decode_vadpcm(std::span<const uint8_t> data, const std::vector<int16_t>& book, int order, int predictors,
                       std::vector<int16_t>& out) {
        if (order != 2 || predictors <= 0 || (int)book.size() < predictors * order * 8) return false;
        int prev[2] = { 0, 0 };
        for (size_t f = 0; f + 9 <= data.size(); f += 9) {
            int scale = data[f] >> 4, p = data[f] & 0xF;
            if (p >= predictors) p = 0;
            const int16_t* row0 = &book[p * 16];
            const int16_t* row1 = row0 + 8;
            for (int half = 0; half < 2; half++) {
                int tmp[8];
                for (int i = 0; i < 8; i++) {
                    uint8_t b = data[f + 1 + half * 4 + i / 2];
                    int n = (i & 1) ? (b & 0xF) : (b >> 4);
                    if (n >= 8) n -= 16;
                    tmp[i] = n * (1 << scale);
                }
                int16_t s[8];
                for (int i = 0; i < 8; i++) {
                    int64_t total = int64_t(row0[i]) * prev[0] + int64_t(row1[i]) * prev[1];
                    for (int k = 0; k < i; k++) total += int64_t(row1[i - 1 - k]) * tmp[k];
                    total += int64_t(tmp[i]) << 11;
                    s[i] = (int16_t)std::clamp<int64_t>(total >> 11, -32768, 32767);
                    out.push_back(s[i]);
                }
                prev[0] = s[6];
                prev[1] = s[7];
            }
        }
        return true;
    }

    // Reads and decodes sound `id` of Rush 2's sound effect bank.
    std::shared_ptr<Sample> load_sample(uint8_t* rdram, int id) {
        uint32_t inst = (uint32_t)MEM_W(0, (int32_t)sound_bank);
        if (inst < 0x80000000 || id < 0 || id >= (int16_t)MEM_H(0, (int32_t)(inst + 0xE))) return nullptr;
        uint32_t sound = (uint32_t)MEM_W(0, (int32_t)(inst + 0x10 + id * 4));
        uint32_t env = (uint32_t)MEM_W(0, (int32_t)sound);
        uint32_t keymap = (uint32_t)MEM_W(0, (int32_t)(sound + 4));
        uint32_t wave = (uint32_t)MEM_W(0, (int32_t)(sound + 8));
        uint32_t base = (uint32_t)MEM_W(0, (int32_t)wave);
        uint32_t len = (uint32_t)MEM_W(0, (int32_t)(wave + 4));
        int type = MEM_BU(0, (int32_t)(wave + 8));
        uint32_t loop = (uint32_t)MEM_W(0, (int32_t)(wave + 0xC));
        uint32_t book = (uint32_t)MEM_W(0, (int32_t)(wave + 0x10));
        std::span<const uint8_t> rom = recomp::get_rom();
        uint32_t offset = base & 0x0FFFFFFF;
        fprintf(stderr, "[engines] Sound 0x%02X: wave 0x%08X len %u type %d, key base %d detune %d, volume %d, sustain %d\n",
                id, base, len, type, MEM_BU(0, (int32_t)(keymap + 4)), (int8_t)MEM_B(0, (int32_t)(keymap + 5)),
                MEM_BU(0, (int32_t)(sound + 0xD)), MEM_BU(0, (int32_t)(env + 0xD)));
        if (type != 0 || book < 0x80000000 || (uint64_t)offset + len > rom.size()) return nullptr;
        int order = (int32_t)MEM_W(0, (int32_t)book);
        int predictors = (int32_t)MEM_W(0, (int32_t)(book + 4));
        if (order != 2 || predictors <= 0 || predictors > 16) return nullptr;
        std::vector<int16_t> coefs(predictors * order * 8);
        for (size_t i = 0; i < coefs.size(); i++) coefs[i] = (int16_t)MEM_H(0, (int32_t)(book + 8 + i * 2));
        auto s = std::make_shared<Sample>();
        if (!decode_vadpcm(rom.subspan(offset, len), coefs, order, predictors, s->pcm) || s->pcm.empty()) return nullptr;
        if (loop >= 0x80000000 && (int32_t)MEM_W(0, (int32_t)(loop + 8)) != 0) {
            s->loop_start = (uint32_t)MEM_W(0, (int32_t)loop);
            s->loop_end = std::min<uint32_t>((uint32_t)MEM_W(0, (int32_t)(loop + 4)), (uint32_t)s->pcm.size());
            if (s->loop_end <= s->loop_start + 1) s->loop_end = 0;
        }
        s->level = MEM_BU(0, (int32_t)(sound + 0xD)) / 127.0f * MEM_BU(0, (int32_t)(env + 0xD)) / 127.0f;
        s->rate = std::exp2((int8_t)MEM_B(0, (int32_t)(keymap + 5)) / 1200.0f);
        return s;
    }

    // A Rush 2 engine layer playing on the host.
    struct Voice {
        std::shared_ptr<Sample> sample;
        double pos = 0.0;
        float step = 1.0f;          // samples per output frame
        float gain[2] = { 0, 0 };   // current, ramped to target
        float target[2] = { 0, 0 };
    };

    std::mutex mutex;
    std::map<int, std::shared_ptr<Sample>> samples; // by sound id (null: unreadable)
    Voice voices[max_cars][2];
    int handles49[max_cars][2];                     // 2049 cars' sound handles
    int levels49[max_cars];
    std::atomic<int64_t> updated_ms = 0;
    std::atomic<uint8_t*> updated_rdram = nullptr;

    int64_t now_ms() {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
    }

    struct Init {
        Init() {
            for (auto& h : handles49) h[0] = h[1] = -1;
            for (int& l : levels49) l = -1;
        }
    } init;

    void stop_car(int car) {
        for (int l = 0; l < 2; l++) {
            voices[car][l] = Voice{};
            if (handles49[car][l] >= 0) audio::sfx_stop(handles49[car][l]);
            handles49[car][l] = -1;
        }
        levels49[car] = -1;
    }

    struct Listener {
        float pos[3], back[3], up[3];
    };

    // A car driven by local player 3 or 4. Rush 2 has engine voices for two players only, so these cars' engines are
    // played here at a player's own volume, centered, instead of as other cars.
    audio::EmitterParams own_engine(float max_volume) {
        return audio::EmitterParams{ max_volume, 0.0f, -1.0f };
    }

    // Rush 2049's law over every local player (split screen sums the volumes and averages pan and surround by them).
    audio::EmitterParams hear(const float pos[3], const std::vector<Listener>& listeners, float max_volume) {
        audio::EmitterParams out{ 0.0f, 0.0f, -1.0f };
        float weight = 0.0f, pan = 0.0f, surround = 0.0f;
        for (const Listener& l : listeners) {
            audio::EmitterParams e = audio::emitter_mix(pos, l.pos, l.back, l.up, range, max_volume);
            out.volume += e.volume;
            pan += e.pan * e.volume;
            surround += e.surround * e.volume;
            weight += e.volume;
        }
        out.volume = std::min(out.volume, 1.0f);
        if (weight > 0.0f) {
            out.pan = pan / weight;
            out.surround = surround / weight;
        }
        return out;
    }

    void update_rush2049(uint8_t* rdram, int car, int level, float rpm, float load, const float pos[3],
                         const std::vector<Listener>& listeners, bool own) {
        rush2::engine2049::LayerMix mix[2];
        if (!rush2::track2049::music_ready() || !rush2::engine2049::layer_mix(level, rpm, load, mix)) {
            stop_car(car);
            return;
        }
        if (level != levels49[car]) {
            stop_car(car);
            levels49[car] = level;
        }
        for (int l = 0; l < 2; l++) {
            int& h = handles49[car][l];
            if (mix[l].sound < 0) continue;
            audio::EmitterParams e = own ? own_engine(mix[l].volume) : hear(pos, listeners, mix[l].volume * other_car_volume);
            if (e.volume <= 0.0f) {
                if (h >= 0) audio::sfx_stop(h);
                h = -1;
                continue;
            }
            if (h < 0 || !audio::sfx_active(h)) {
                h = audio::sfx_start(mix[l].sound, e.volume, e.pan, mix[l].pitch, e.surround);
            }
            else {
                audio::sfx_update(h, e.volume, e.pan, mix[l].pitch, e.surround);
            }
        }
        rush2::track2049::effects_running(rdram);
    }

    void update_rush2(uint8_t* rdram, int car, int engine, uint32_t c, const float pos[3],
                      const std::vector<Listener>& listeners, bool own) {
        if (levels49[car] >= 0) stop_car(car);
        float r = float(uint32_t(std::abs((int)(int16_t)MEM_H(0, (int32_t)(c + car_rpm)) * f32_at(rdram, rpm_scale))) & 0x7FFF);
        float l = float(uint32_t((int16_t)(int32_t)(f32_at(rdram, c + car_load) * f32_at(rdram, load_scale))) & 0x7FFF);
        int ri = std::clamp(int(r / 1000.0f), 0, 10);
        int li = std::clamp(int((l + 80.0f) * 0.025f), 0, 12);
        float option_volume = (int8_t)MEM_B(0, (int32_t)engine_volume) / 40.0f;
        for (int layer = 0; layer < 2; layer++) {
            Voice& v = voices[car][layer];
            int id = (int32_t)MEM_W(0, (int32_t)(engine_sounds + (engine * 2 + layer) * 4));
            if (id < 0 || (id & ~0x200) == turbo_sound) {
                v = Voice{};
                continue;
            }
            id &= ~0x200;
            auto it = samples.find(id);
            if (it == samples.end()) it = samples.emplace(id, load_sample(rdram, id)).first;
            if (it->second == nullptr) {
                v = Voice{};
                continue;
            }
            float base = f32_at(rdram, engine_bases + (engine * 2 + layer) * 4);
            float pitch = base > 0.0f ? std::min(r / base, 2.0f) : 1.0f;
            float a = f32_at(rdram, rpm_table + (layer * 11 + ri) * 4);
            float b = f32_at(rdram, load_table + (layer * 13 + li) * 4);
            int volume = int(float(int(a * b * 2000.0f + 24000.0f)) * option_volume);
            audio::EmitterParams e = own ? own_engine(1.0f) : hear(pos, listeners, other_car_volume);
            float gain = std::clamp(volume / 32767.0f, 0.0f, 1.0f) * it->second->level * e.volume;
            float angle = (std::clamp(e.pan, -1.0f, 1.0f) + 1.0f) * 0.25f * 3.14159265f;
            if (v.sample != it->second) {
                v = Voice{};
                v.sample = it->second;
            }
            v.step = pitch * it->second->rate;
            v.target[0] = gain * std::cos(angle);
            v.target[1] = gain * std::sin(angle);
        }
    }
}

void rush2::car_engines::set_enabled(bool enabled) {
    option = enabled;
    if (!enabled) stop();
}

bool rush2::car_engines::enabled() {
    return option;
}

void rush2::car_engines::update(uint8_t* rdram) {
    int local = std::clamp<int>((int16_t)MEM_H(0, (int32_t)num_players), 1, max_players);
    std::vector<Listener> listeners;
    for (int s = 0; s < local; s++) {
        Listener l;
        uint32_t c = physics_cars + MEM_BU(0, (int32_t)(players + s * player_size)) * physics_car_size;
        for (int i = 0; i < 3; i++) {
            l.pos[i] = f32_at(rdram, c + car_position + i * 4);
            l.up[i] = f32_at(rdram, cameras + s * camera_size + 0xC + i * 4);
            l.back[i] = f32_at(rdram, cameras + s * camera_size + 0x18 + i * 4);
        }
        listeners.push_back(l);
    }
    bool muted = MEM_BU(0, (int32_t)engines_muted) != 0;
    bool extra_player[max_cars] = {};
    for (int s = 2; s < local; s++) {
        int car = MEM_BU(0, (int32_t)(players + s * player_size));
        if (car < max_cars) extra_player[car] = true;
    }
    std::lock_guard lock{ mutex };
    for (int car = 0; car < max_cars; car++) {
        uint32_t c = physics_cars + car * physics_car_size;
        // Players 1 and 2's cars have the game's own engines; the other cars' only play with the option on.
        bool skipped = extra_player[car] ? false : !option || (int8_t)MEM_B(0, (int32_t)(c + car_driver)) == 2;
        bool off = (int16_t)MEM_H(0, (int32_t)(c + car_active)) == 0 || skipped || muted || (int8_t)MEM_B(0, (int32_t)(c + car_engine_off)) != 0 ||
                   (int8_t)MEM_B(0, (int32_t)(car_states + car * car_state_size + 0x343)) > 0;
        if (off) {
            stop_car(car);
            continue;
        }
        float pos[3];
        for (int i = 0; i < 3; i++) pos[i] = f32_at(rdram, c + car_position + i * 4);
        int level = rush2::car2049::engine_level(rdram, c);
        if (level >= 0) {
            float load = f32_at(rdram, c + car_load);
            update_rush2049(rdram, car, level, (float)(int16_t)MEM_H(0, (int32_t)(c + car_rpm)), load, pos, listeners,
                            extra_player[car]);
            continue;
        }
        int type = MEM_BU(0, (int32_t)(c + car_type));
        int engine = type < rush2::car2049::types ? (int8_t)MEM_B(0, (int32_t)(default_engines + type)) : -1;
        if (engine < 0 || engine >= 10) {
            stop_car(car);
            continue;
        }
        update_rush2(rdram, car, engine, c, pos, listeners, extra_player[car]);
    }
    updated_rdram = rdram;
    updated_ms = now_ms();
}

void rush2::car_engines::stop() {
    std::lock_guard lock{ mutex };
    for (int car = 0; car < max_cars; car++) stop_car(car);
}

void rush2::car_engines::mix(float* out, size_t sample_count, uint32_t, float scale) {
    // Silent while nothing updates them (paused).
    if (rush2::track2049::sounds_paused(updated_rdram.load(), updated_ms.load())) return;
    std::lock_guard lock{ mutex };
    size_t frames = sample_count / 2;
    constexpr float ramp = 1.0f / 256.0f; // about 5 ms
    for (auto& car : voices) {
        for (Voice& v : car) {
            if (v.sample == nullptr) continue;
            const std::vector<int16_t>& pcm = v.sample->pcm;
            double size = (double)pcm.size();
            for (size_t f = 0; f < frames; f++) {
                for (int ch = 0; ch < 2; ch++) v.gain[ch] += (v.target[ch] - v.gain[ch]) * ramp;
                if (v.pos >= size - 1) {
                    if (v.sample->loop_end == 0) break;
                    v.pos -= double(v.sample->loop_end - v.sample->loop_start);
                }
                size_t i = (size_t)v.pos;
                float t = float(v.pos - (double)i);
                float s = pcm[i] + (pcm[std::min(i + 1, pcm.size() - 1)] - pcm[i]) * t;
                out[f * 2] += s * v.gain[0] * scale;
                out[f * 2 + 1] += s * v.gain[1] * scale;
                v.pos += v.step;
                if (v.sample->loop_end != 0 && v.pos >= v.sample->loop_end) {
                    v.pos -= double(v.sample->loop_end - v.sample->loop_start);
                }
            }
        }
    }
}
