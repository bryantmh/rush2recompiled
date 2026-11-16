// Fancy UFO (Games tab, SF Rush PlayStation section; include/fancy_ufo.h, docs/sfrush_ps1.md#fancy-ufo). The PlayStation
// SF Rush mod's UFO (src/sfrush_ps1/ps1_cars.cpp builds it as the mod car "PS1UFO", named UFO) gets a starship's
// engine and horn, a hover glow under its three pods, and thruster jets while its hidden wings steer it.
//
// Engine and horn: the car select's ENGINE and HORN rows (menu_screen_car_select_frame 0x803B9478, cases 8 and 7 of
// its row switch) gain a value on the UFO, past Rush 2's own: ENGINE 10, WARP CORE (Rush 2 wraps at 10) and HORN 9,
// HAILING (Rush 2 wraps at 9). They are the UFO's defaults. Rush 2's sound player can't play them (the engine's loop
// table 0x800BD1FC and the horns' sound ids 0x800C1000 have no entry past their last), so:
//   - the race's engine start (func_8008C61C) is skipped for ENGINE values past 9 (src/rush2049/engine2049.cpp), and
//     src/car_engines.cpp plays the WARP CORE for the car, as it plays California Speed's engines;
//   - the horn plays HAILING here and the menu_play_sound call that follows is skipped (the HORN row at 0x803B9DD8,
//     sounding it 0.6 s; in a race car_sound_update_all 0x80067558): HAILING sounds on while a race's horn button stays
//     held (0x800674F0, every tick) and fades out when it's let go.
//   - A player record holds Rush 2's own values only (a byte per row: +0x589 HORN, +0x58A ENGINE in the type's 13-byte
//     block, which the game's getters and setters read and write; another build without this would read garbage).
//     As src/paint_colors.cpp does for its extra colors, a pick of WARP CORE or HAILING is saved as the type's default
//     value (row 0 of the per-type table) with the pick kept in the saves' "fancy_ufo" section, keyed by the record
//     address (rush2::car2049::record_address); the getters (carselect_lookup_value_d 0x800B2554 ENGINE,
//     carselect_lookup_value_e 0x800B2578 HORN) give the pick back while the option is on. A record with no pick
//     yet gets the new ones.
//   - The rows' names: the value text of the row (carselect_text_cb_car_title 0x803BC048, case 6 HORN reads 0x800C49D4
//     + language x 36 + value x 4, case 7 ENGINE 0x800C49F8 + language x 40 + value x 4) is swapped at 0x803BC63C
//     (src/rush2049/car2049.cpp rush2_car49_option_text) for labels written to 0x802259C0 / 0x802259D0.
//   The sounds are made here (no recorded sound): the engine is two looped layers, a low pulsing hum and a high
//   shimmering whine, retuned by the car's speed; the horn a rising chirp into a warbling held tone, looped.
//
// Hover glow: each pod (the three hexagonal pods under the saucer, their tips in PS1UFO.glb at the knob table below,
// 1/16 ft) shines a cone of light down to the ground, with a bright core, and rings of light run down it. They are
// rush2::battle_render models of the port's own (UFO_HOVER, UFO_RING: vertex colored, unlit, unfogged, blended without
// writing depth, colored by the slot's PRIM), placed with the car's body pose before each view (pvs_apply 0x8007C27C,
// as src/car_lights.cpp does) in slots 332-387. Shown for any race car that is the UFO, but not a wrecked car, a car
// out of the race or a ghost.
//
// Thruster jets: UFO_JET models of the port's own on the rim's four nozzles, placed with the glow (place_jets).
//
// Test aids: RUSH2_UFO_SOUND_DUMP=<folder> writes the three sounds as .wav files at startup.

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <complex>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <thread>

#include "json/json.hpp"
#include "librecomp/config.hpp"
#include "recomp.h"

#include "battle.h"
#include "battle_render.h"
#include "car2049.h"
#include "car_engines.h"
#include "car_types.h"
#include "data_files.h"
#include "fancy_ufo.h"
#include "ghost.h"
#include "options_page.h"
#include "own_model_dl.h"
#include "rush2_hooks.h"
#include "wings_internal.h"


namespace ct = rush2::car_types;

namespace {
    const std::string option_id = "sfrush_ps1_fancy_ufo";
    std::atomic_bool option = true;

    constexpr float pi = 3.14159265358979f;

    // ---------------------------------------------------------------------------------------------------------------
    // The sounds.

    // In-place radix-2 FFT (inverse: unscaled).
    void fft(std::vector<std::complex<double>>& a, bool inverse) {
        size_t n = a.size();
        for (size_t i = 1, j = 0; i < n; i++) {
            size_t bit = n >> 1;
            for (; j & bit; bit >>= 1) j ^= bit;
            j ^= bit;
            if (i < j) std::swap(a[i], a[j]);
        }
        for (size_t len = 2; len <= n; len <<= 1) {
            double ang = 2.0 * 3.14159265358979323846 / double(len) * (inverse ? 1.0 : -1.0);
            std::complex<double> wl(std::cos(ang), std::sin(ang));
            for (size_t i = 0; i < n; i += len) {
                std::complex<double> w(1.0, 0.0);
                for (size_t k = 0; k < len / 2; k++) {
                    std::complex<double> u = a[i + k], v = a[i + k + len / 2] * w;
                    a[i + k] = u + v;
                    a[i + k + len / 2] = u - v;
                    w *= wl;
                }
            }
        }
    }

    // Noise between lo and hi Hz that loops seamlessly over n samples (random phases on the loop's own bins), at rms.
    std::vector<double> band_noise(size_t n, double lo, double hi, double rms, uint32_t seed) {
        std::vector<std::complex<double>> spec(n);
        double df = double(rush2::fancy_ufo::sound_rate) / double(n);
        uint32_t s = seed;
        for (size_t k = 1; k < n / 2; k++) {
            double f = double(k) * df;
            if (f < lo || f > hi) continue;
            s = s * 1664525u + 1013904223u;
            double phase = double(s >> 8) / double(1u << 24) * 2.0 * 3.14159265358979323846;
            // Soft edges, so the band has no ringing tones at its ends.
            double edge = std::min({ 1.0, (f - lo) / (lo * 0.25 + 1.0), (hi - f) / (hi * 0.25 + 1.0) });
            spec[k] = std::polar(edge, phase);
            spec[n - k] = std::conj(spec[k]);
        }
        fft(spec, true);
        std::vector<double> out(n);
        double sum = 0.0;
        for (size_t i = 0; i < n; i++) {
            out[i] = spec[i].real();
            sum += out[i] * out[i];
        }
        double now = std::sqrt(sum / double(n));
        if (now > 0.0) {
            for (double& v : out) v *= rms / now;
        }
        return out;
    }

    std::vector<int16_t> to_pcm(const std::vector<double>& x, double peak) {
        double m = 0.0;
        for (double v : x) m = std::max(m, std::fabs(v));
        std::vector<int16_t> out(x.size());
        double k = m > 0.0 ? peak * 32767.0 / m : 0.0;
        for (size_t i = 0; i < x.size(); i++) out[i] = int16_t(std::lround(std::clamp(x[i] * k, -32767.0, 32767.0)));
        return out;
    }

    // The loops are 65536 samples (2.048 s): every tone sits on a whole number of cycles of the loop (bin b is
    // b x 0.488 Hz), so the loop has no seam.
    constexpr size_t loop_size = 65536;

    double tone(size_t i, int bin, double phase = 0.0, double pm = 0.0) {
        return std::sin(2.0 * 3.14159265358979323846 * double(bin) * double(i) / double(loop_size) + phase + pm);
    }

    // The hum: a low drone at 55 Hz and its harmonics, pairs a bin or three apart beating slowly, pulsing twice a
    // second, over a little low rumble.
    std::vector<int16_t> make_hum() {
        std::vector<double> rumble = band_noise(loop_size, 60.0, 420.0, 0.05, 0x5EED0001u);
        std::vector<double> x(loop_size);
        for (size_t i = 0; i < loop_size; i++) {
            double h = 0.30 * tone(i, 113) + 0.20 * tone(i, 226, 0.4) + 0.12 * tone(i, 228, 1.1) + 0.10 * tone(i, 339, 2.0) +
                       0.07 * tone(i, 452, 0.7) + 0.05 * tone(i, 455, 2.6) + 0.03 * tone(i, 678, 1.9);
            double pulse = 0.8 + 0.2 * tone(i, 4);
            x[i] = h * pulse + rumble[i];
        }
        return to_pcm(x, 0.55);
    }

    // The whine: a shimmering chord near 440, 660 and 880 Hz (phase-modulated at 5.9 Hz) with an octave below, and a
    // breath of high hiss.
    std::vector<int16_t> make_whine() {
        std::vector<double> hiss = band_noise(loop_size, 1800.0, 5200.0, 0.035, 0x5EED0002u);
        std::vector<double> x(loop_size);
        for (size_t i = 0; i < loop_size; i++) {
            double pm = tone(i, 12);
            double c = 0.25 * tone(i, 901, 0.0, 0.5 * pm) + 0.15 * tone(i, 1352, 1.0, 0.75 * pm) +
                       0.08 * tone(i, 1803, 2.0, 1.0 * pm) + 0.07 * tone(i, 450, 0.3);
            x[i] = c * (0.85 + 0.15 * tone(i, 2)) + hiss[i];
        }
        return to_pcm(x, 0.5);
    }

    // The horn: a rising chirp (F#5 to E6, 0.14 s) into a held tone that loops for as long as the horn is held: E6
    // and a fifth above it (B6) over the octave below, warbling (vibrato at 5.9 Hz, a pulse at 7.8 Hz). The loop is
    // horn_loop samples (0.512 s, bins of 1.95 Hz) with every tone and wobble on whole bins, so it has no seam; the
    // chirp's held part is the loop's own end, so it runs into the loop without a join.
    constexpr size_t horn_loop = 16384;
    constexpr size_t horn_intro = size_t(0.14 * 32000);   // sound_rate; the loop starts here

    double horn_held(size_t i) {
        auto t = [&](int bin, double phase, double pm) {
            return std::sin(2.0 * 3.14159265358979323846 * double(bin) * double(i % horn_loop) / double(horn_loop) + phase + pm);
        };
        double wob = t(3, 0.0, 0.0);   // 5.86 Hz: about 1.2% of each tone's pitch
        double held = 0.55 * t(675, 0.0, 2.7 * wob) + 0.18 * t(1350, 0.6, 5.4 * wob) + 0.30 * t(1012, 1.3, 4.0 * wob) +
                      0.16 * t(338, 0.4, 1.35 * wob);
        return held * (0.8 + 0.2 * t(4, 0.0, 0.0));
    }

    std::vector<int16_t> make_horn() {
        const double rate = rush2::fancy_ufo::sound_rate;
        std::vector<double> x(horn_intro + horn_loop, 0.0);
        double phase = 0.0;
        for (size_t i = 0; i < horn_intro; i++) {
            double u = double(i) / double(horn_intro);
            // The held tone fades in under the chirp, and the chirp out by the loop's start.
            double held = horn_held(horn_loop - horn_intro + i) * u * u;
            double f = 740.0 + (1318.4 - 740.0) * std::sqrt(u);
            phase += 2.0 * 3.14159265358979323846 * f / rate;
            double env = std::min(1.0, double(i) / rate / 0.006) * (1.0 - u * u);
            x[i] = held + 0.8 * env * (std::sin(phase) + 0.3 * std::sin(2.0 * phase));
        }
        for (size_t i = 0; i < horn_loop; i++) x[horn_intro + i] = horn_held(i);
        return to_pcm(x, 0.6);
    }

    // Made once by a worker started with the option (init_config): about 20 ms, which the game thread mustn't wait for.
    std::once_flag sounds_once;
    std::atomic_bool sounds_ready = false;
    std::vector<int16_t> hum, whine, horn_pcm;
    const std::vector<int16_t> no_sound;

    void dump_wav(const std::string& path, const std::vector<int16_t>& pcm) {
        FILE* f = std::fopen(path.c_str(), "wb");
        if (f == nullptr) return;
        auto u32 = [&](uint32_t v) { std::fwrite(&v, 4, 1, f); };
        auto u16 = [&](uint16_t v) { std::fwrite(&v, 2, 1, f); };
        uint32_t bytes = uint32_t(pcm.size() * 2);
        std::fwrite("RIFF", 1, 4, f);
        u32(36 + bytes);
        std::fwrite("WAVEfmt ", 1, 8, f);
        u32(16);
        u16(1);
        u16(1);
        u32(rush2::fancy_ufo::sound_rate);
        u32(rush2::fancy_ufo::sound_rate * 2);
        u16(2);
        u16(16);
        std::fwrite("data", 1, 4, f);
        u32(bytes);
        std::fwrite(pcm.data(), 2, pcm.size(), f);
        std::fclose(f);
    }

    void make_sounds() {
        std::call_once(sounds_once, [] {
            auto start = std::chrono::steady_clock::now();
            hum = make_hum();
            whine = make_whine();
            horn_pcm = make_horn();
            double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
            std::printf("[fancy_ufo] sounds made in %.1f ms\n", ms);
            if (const char* dir = std::getenv("RUSH2_UFO_SOUND_DUMP"); dir != nullptr && *dir) {
                dump_wav(std::string(dir) + "/ufo_hum.wav", hum);
                dump_wav(std::string(dir) + "/ufo_whine.wav", whine);
                dump_wav(std::string(dir) + "/ufo_horn.wav", horn_pcm);
            }
            sounds_ready = true;
        });
    }

    const std::vector<int16_t>& ready_or_none(const std::vector<int16_t>& v) {
        return sounds_ready.load(std::memory_order_acquire) ? v : no_sound;
    }

    // ---------------------------------------------------------------------------------------------------------------
    // The saved picks.

    // (record address of the row's byte, field 0 ENGINE / 1 HORN) -> 1 the UFO's own sound picked, 0 one of Rush 2's.
    const std::string section = "fancy_ufo";
    constexpr int field_engine = 0, field_horn = 1;
    std::mutex store_mutex;
    std::map<std::pair<uint32_t, int>, int> picks;
    bool store_loaded = false;
    constexpr uint32_t block_span = 0x584 + ct::types * 13;   // a record's blocks, the extra types' included

    void load_store() {
        if (store_loaded) return;
        store_loaded = true;
        nlohmann::json j = nlohmann::json::parse(rush2::data_files::read(rush2::data_files::File::Saves, section), nullptr, false);
        if (!j.is_object() || !j.contains("picks") || !j["picks"].is_object()) return;
        for (auto& [key, value] : j["picks"].items()) {
            size_t colon = key.find(':');
            if (colon == std::string::npos || !value.is_number_integer()) continue;
            uint32_t at = (uint32_t)std::strtoul(key.substr(0, colon).c_str(), nullptr, 16);
            int field = std::atoi(key.c_str() + colon + 1);
            if (field == field_engine || field == field_horn) picks[{ at, field }] = value.get<int>() != 0 ? 1 : 0;
        }
    }

    void save_store() {
        nlohmann::json out = nlohmann::json::object();
        for (auto& [key, own] : picks) {
            char name[24];
            std::snprintf(name, sizeof(name), "%08X:%d", key.first, key.second);
            out[name] = own;
        }
        rush2::data_files::write(rush2::data_files::File::Saves, section, nlohmann::json{ { "version", 1 }, { "picks", out } }.dump());
    }

    // The row's own value for the UFO, and the per-type table (row 0 holds each type's default: the stand-in).
    int own_value(int field) { return field == field_engine ? rush2::fancy_ufo::engine_value : rush2::fancy_ufo::horn_value; }
    uint32_t table_of(int field) { return ct::table(field == field_engine ? ct::engine : ct::horn); }
    int stand_in(uint8_t* rdram, int field, int type) {
        int v = (int8_t)MEM_B(0, (int32_t)(table_of(field) + uint32_t(type)));
        return std::clamp(v, 0, own_value(field) - 1);
    }

    // ---------------------------------------------------------------------------------------------------------------
    // The horn.

    std::atomic_bool skip_next_sound = false;

    static_assert(rush2::fancy_ufo::sound_rate == 32000, "horn_intro assumes it");

    // Sounds HAILING for `hold` seconds (on again if it already sounds).
    void play_horn(uint8_t* rdram, float hold) {
        rush2::car_engines::play_horn(rdram, ready_or_none(horn_pcm), rush2::fancy_ufo::sound_rate, 1.0f,
                                      uint32_t(horn_intro), hold);
    }
    constexpr float menu_hold = 0.6f;   // the car select's HORN row sounds it this long
    constexpr float tick_hold = 0.1f;   // a race's held horn button, renewed every physics tick

    int horn_before = -1;

    // The row labels (Rush 2's strings: upper case ASCII, zero ended), in RDRAM the car options leave free.
    constexpr uint32_t engine_label = 0x802259C0, horn_label = 0x802259D0;
    void put_label(uint8_t* rdram, uint32_t at, const char* text) {
        for (int i = 0; i < 16; i++) {
            char c = i < 15 ? text[std::min<size_t>(i, std::strlen(text))] : 0;
            MEM_B(i, (int32_t)at) = (int8_t)c;
        }
    }

    // ---------------------------------------------------------------------------------------------------------------
    // The hover glow.

    // The pods' tips in PS1UFO.glb (1/16 ft; x right, y up, z forward): the lowest vertex of each pod, measured on the
    // UFO at Rush 2's width cap ({ 25.84, 0.45 }, { -14.15, 21.81 }, { -11.56, -23.29 }, 4.2 above the origin;
    // tools/glb_view.py PS1UFO.glb --pitch -89 shows them), at its size now (rush2::fancy_ufo::size): across about the
    // origin, up about the axle height.
    constexpr float grow = rush2::fancy_ufo::size;
    constexpr float up_scaled(float y) { return rush2::fancy_ufo::axle_y + (y - rush2::fancy_ufo::axle_y) * grow; }
    constexpr float pods[3][2] = { { 25.84f * grow, 0.45f * grow }, { -14.15f * grow, 21.81f * grow }, { -11.56f * grow, -23.29f * grow } };
    constexpr float pod_tip = up_scaled(4.2f);

    // Spare RDRAM below 16 MB (the list in src/controls_menu.cpp): the beams' vertices and list, the ring's.
    constexpr uint32_t beam_vertices = 0x80E15680;   // 3 x 29 x 16 = 0x570
    constexpr uint32_t beam_dl = 0x80E15C00;         // to 0x80E16000
    constexpr uint32_t ring_vertices = 0x80E14FC0;   // 30 x 16 = 0x1E0
    constexpr uint32_t ring_dl = 0x80E15200;         // to 0x80E15600
    constexpr const char* beam_model = "UFO_HOVER";
    constexpr const char* ring_model = "UFO_RING";
    // The thruster jet's, in the spare ends of the blocks above (the ring's list ends by 0x80E15320, the beams' by
    // 0x80E15E18).
    constexpr uint32_t jet_vertices = 0x80E15320;    // 19 x 16 = 0x130, to 0x80E15450
    constexpr uint32_t jet_dl = 0x80E15E40;          // to 0x80E15F40
    constexpr const char* jet_model = "UFO_JET";

    constexpr int max_cars = 8;
    constexpr int rings_per_pod = 2;
    constexpr int first_slot = 332;   // battle_render slots 332-339 the beams, 340-387 the rings, 388-419 the jets
    constexpr int ring_slot = first_slot + max_cars;
    constexpr int jet_slot = ring_slot + max_cars * 3 * rings_per_pod;
    static_assert(jet_slot + max_cars * rush2::fancy_ufo::thrusters <= rush2::battle_render::max_slots);

    constexpr uint32_t game_state = 0x8010C0D0;
    constexpr uint32_t cars = 0x800F5470, car_size = 0x81C;
    constexpr uint32_t car_type = 0x7EA;     // u8
    constexpr uint32_t car_wrecked = 0x648;  // s8
    constexpr uint32_t car_active = 0x7E4;   // s16, nonzero for a car in the race
    constexpr uint32_t car_flags = 0x7F4;    // 0x10: the car isn't drawn (after a wreck, until its reset)

    // The glow's color (PRIM): a pale cyan.
    constexpr uint32_t glow_rgb = 0x70D8FF00;
    // The jets': a blue-tinged white.
    constexpr uint32_t jet_rgb = 0xE6F4FF00;

    bool models_written = false;
    bool shown[max_cars] = {};
    bool jets_shown[max_cars][rush2::fancy_ufo::thrusters] = {};

    using rush2::own_model::put_vertex;
    using rush2::own_model::Dl;

    // Each pod: an outer soft cone (8 sides, radius 9 at the pod down to 20 at 16 below its tip, alpha 150 to 0), a
    // bright inner one (6 sides, radius 4 to 7, alpha 255 to 60), and a disc closing the inner cone's top under the pod.
    // Vertices per pod: 0-7 outer top, 8-15 outer bottom, 16-21 inner top, 22-27 inner bottom, 28 the disc's middle.
    void write_beam(uint8_t* rdram) {
        constexpr int per_pod = 29;
        Dl dl{ rdram, beam_dl };
        dl.begin();
        for (int p = 0; p < 3; p++) {
            const float cx = pods[p][0], cz = pods[p][1];
            const float top = pod_tip + 1.5f * grow, bottom = pod_tip - 16.0f * grow;
            uint32_t v = beam_vertices + uint32_t(p * per_pod) * 16;
            for (int i = 0; i < 8; i++) {
                float a = 2.0f * pi * float(i) / 8.0f;
                put_vertex(rdram, v + uint32_t(i) * 16, cx + 9.0f * grow * std::cos(a), top, cz + 9.0f * grow * std::sin(a), 150);
                put_vertex(rdram, v + uint32_t(8 + i) * 16, cx + 20.0f * grow * std::cos(a), bottom, cz + 20.0f * grow * std::sin(a), 0);
            }
            for (int i = 0; i < 6; i++) {
                float a = 2.0f * pi * (float(i) + 0.5f) / 6.0f;
                put_vertex(rdram, v + uint32_t(16 + i) * 16, cx + 4.0f * grow * std::cos(a), top - 0.5f * grow, cz + 4.0f * grow * std::sin(a), 255);
                put_vertex(rdram, v + uint32_t(22 + i) * 16, cx + 7.0f * grow * std::cos(a), bottom, cz + 7.0f * grow * std::sin(a), 60);
            }
            put_vertex(rdram, v + 28 * 16, cx, pod_tip - 0.2f * grow, cz, 255);
            dl.vtx(v, per_pod);
            for (int i = 0; i < 8; i++) {
                int j = (i + 1) % 8;
                dl.tri2(i, j, 8 + j, i, 8 + j, 8 + i);
            }
            for (int i = 0; i < 6; i++) {
                int j = (i + 1) % 6;
                dl.tri2(16 + i, 16 + j, 22 + j, 16 + i, 22 + j, 22 + i);
            }
            for (int i = 0; i < 6; i += 2) {
                dl.tri2(28, 16 + i, 16 + (i + 1) % 6, 28, 16 + (i + 1) % 6, 16 + (i + 2) % 6);
            }
        }
        dl.end();
    }

    // A flat ring of light, radius 16 (1 ft) at its brightest, soft on both edges: 10 segments of three circles
    // (radius 11 alpha 0, 16 alpha 255, 19 alpha 0).
    void write_ring(uint8_t* rdram) {
        constexpr int segments = 10;
        constexpr float radii[3] = { 11.0f, 16.0f, 19.0f };
        constexpr uint8_t alphas[3] = { 0, 255, 0 };
        for (int i = 0; i < segments; i++) {
            float a = 2.0f * pi * float(i) / float(segments);
            for (int r = 0; r < 3; r++) {
                put_vertex(rdram, ring_vertices + uint32_t(r * segments + i) * 16, radii[r] * std::cos(a), 0.0f,
                           radii[r] * std::sin(a), alphas[r]);
            }
        }
        Dl dl{ rdram, ring_dl };
        dl.begin();
        dl.vtx(ring_vertices, 3 * segments);
        for (int band = 0; band < 2; band++) {
            for (int i = 0; i < segments; i++) {
                int j = (i + 1) % segments;
                int a = band * segments + i, b = band * segments + j, c = (band + 1) * segments + j, d = (band + 1) * segments + i;
                dl.tri2(a, b, c, a, c, d);
            }
        }
        dl.end();
    }

    // A thruster's jet, pointing along +y from its nozzle at the origin (1/16 ft): a soft plume (6 sides, radius 3 at
    // the nozzle, alpha 230, to 8 at 36 out, alpha 0) around a bright core (6 sides, radius 1.4, alpha 255, narrowing
    // to a point 22 out, alpha 0), the core's nozzle ring fanned into a disc. Its slot's matrix stretches it along the
    // jet. Vertices: 0-5 plume nozzle, 6-11 plume end, 12-17 core nozzle, 18 the core's tip.
    void write_jet(uint8_t* rdram) {
        constexpr int sides = 6;
        for (int i = 0; i < sides; i++) {
            float a = 2.0f * pi * float(i) / float(sides);
            float c = std::cos(a), sn = std::sin(a);
            put_vertex(rdram, jet_vertices + uint32_t(i) * 16, 3.0f * c, 0.0f, 3.0f * sn, 230);
            put_vertex(rdram, jet_vertices + uint32_t(sides + i) * 16, 8.0f * c, 36.0f, 8.0f * sn, 0);
            put_vertex(rdram, jet_vertices + uint32_t(2 * sides + i) * 16, 1.4f * c, 0.5f, 1.4f * sn, 255);
        }
        put_vertex(rdram, jet_vertices + uint32_t(3 * sides) * 16, 0.0f, 22.0f, 0.0f, 0);
        Dl dl{ rdram, jet_dl };
        dl.begin();
        dl.vtx(jet_vertices, 3 * sides + 1);
        for (int i = 0; i < sides; i++) {
            int j = (i + 1) % sides;
            dl.tri2(i, j, sides + j, i, sides + j, sides + i);
        }
        for (int i = 0; i < sides; i += 2) {
            int a = 2 * sides + i, b = 2 * sides + (i + 1) % sides, c = 2 * sides + (i + 2) % sides;
            dl.tri2(a, b, 3 * sides, b, c, 3 * sides);
        }
        // The nozzle's disc: the core's ring fanned (4 triangles).
        dl.tri2(2 * sides, 2 * sides + 1, 2 * sides + 2, 2 * sides, 2 * sides + 2, 2 * sides + 3);
        dl.tri2(2 * sides, 2 * sides + 3, 2 * sides + 4, 2 * sides, 2 * sides + 4, 2 * sides + 5);
        dl.end();
    }

    void hide_jets(int car) {
        for (int t = 0; t < rush2::fancy_ufo::thrusters; t++) {
            if (!jets_shown[car][t]) continue;
            jets_shown[car][t] = false;
            rush2::battle_render::hide(jet_slot + car * rush2::fancy_ufo::thrusters + t);
        }
    }

    void hide_car(int car) {
        hide_jets(car);
        if (!shown[car]) return;
        shown[car] = false;
        rush2::battle_render::hide(first_slot + car);
        for (int k = 0; k < 3 * rings_per_pod; k++) rush2::battle_render::hide(ring_slot + car * 3 * rings_per_pod + k);
    }

    // The RCS jets while the hidden wings hold the UFO up: the stick's steering as thrusts from four nozzles on the
    // rim, each the way that turns the saucer as the stick does. The wing torque (rush2_wings_torque) is pitch -=
    // stick Y, roll += stick X x mirror; measured in game (RUSH2_UFO_LOG=1), a negative pitch torque drops the nose
    // and a positive roll torque drops the right side, so nose_up = the pitch torque and right_up = -the roll torque.
    // A nozzle's jet fires down to lift its side and up to lower it, tilted a little out from the rim. Placed with the
    // drawn body's pose before each view, as the glow, so the jets sit on the saucer whatever its speed.
    constexpr float jet_dead = 0.15f;   // stick travel that fires no jet

    void place_jets(uint8_t* rdram, int car, const float pos[3], const float m[9], float now) {
        rush2::wings::Pose pose;
        if (!rush2::wings::enabled() || !rush2::wings::get_pose(rdram, car, pose)) {
            hide_jets(car);
            return;
        }
        float nose_up = -pose.stick_y, right_up = -pose.stick_x * pose.mirror;
        // Right, left, front, back (rush2::fancy_ufo::thruster): along up, -1 down (lifts its side).
        const float jets[rush2::fancy_ufo::thrusters] = { -right_up, right_up, -nose_up, nose_up };
        const float* right = m;
        const float* up = m + 3;
        const float* fwd = m + 6;
        static const bool log_on = std::getenv("RUSH2_UFO_LOG") != nullptr;
        static int logged = 0;
        if (log_on && (logged++ % 10) == 0) {
            std::printf("[fancy_ufo] car %d stick %.2f %.2f: nose up %+.2f right up %+.2f; axes right y %.3f fwd y %.3f\n",
                        car, pose.stick_x, pose.stick_y, nose_up, right_up, right[1], fwd[1]);
            std::fflush(stdout);
        }
        for (int t = 0; t < rush2::fancy_ufo::thrusters; t++) {
            const int slot = jet_slot + car * rush2::fancy_ufo::thrusters + t;
            float strength = std::min(1.0f, std::fabs(jets[t]));
            if (strength < jet_dead) {
                if (jets_shown[car][t]) rush2::battle_render::hide(slot);
                jets_shown[car][t] = false;
                continue;
            }
            float dir = jets[t] > 0.0f ? 1.0f : -1.0f;
            float local[3];
            rush2::fancy_ufo::thruster(t, local);
            float len = std::sqrt(local[0] * local[0] + local[2] * local[2]);
            float at[3], out[3], jet[3], side[3], third[3];
            for (int k = 0; k < 3; k++) {
                at[k] = pos[k] + right[k] * local[0] + up[k] * local[1] + fwd[k] * local[2];
                out[k] = (right[k] * local[0] + fwd[k] * local[2]) / len;
                jet[k] = up[k] * dir + out[k] * 0.3f;
            }
            float jl = std::sqrt(jet[0] * jet[0] + jet[1] * jet[1] + jet[2] * jet[2]);
            for (float& v : jet) v /= jl;
            // side = up x out (across the jet), third = side x jet.
            side[0] = up[1] * out[2] - up[2] * out[1];
            side[1] = up[2] * out[0] - up[0] * out[2];
            side[2] = up[0] * out[1] - up[1] * out[0];
            third[0] = side[1] * jet[2] - side[2] * jet[1];
            third[1] = side[2] * jet[0] - side[0] * jet[2];
            third[2] = side[0] * jet[1] - side[1] * jet[0];
            // Flicker: two fast waves, offset per nozzle.
            float ph = now * 2.0f * pi + float(car * 4 + t) * 1.7f;
            float flicker = 1.0f + 0.10f * std::sin(ph * 23.0f) + 0.06f * std::sin(ph * 37.0f + 0.8f);
            float length = (0.45f + 0.55f * strength) * flicker * grow;
            float width = (0.8f + 0.2f * strength) * grow;
            float jm[9];
            for (int k = 0; k < 3; k++) {
                jm[k] = side[k] * width;
                jm[3 + k] = jet[k] * length;
                jm[6 + k] = third[k] * width;
            }
            uint32_t alpha = uint32_t(std::clamp(0.75f + 0.25f * strength * flicker, 0.0f, 1.0f) * 255.0f);
            rush2::battle_render::place(slot, jet_model, jm, at, jet_rgb | alpha);
            jets_shown[car][t] = true;
        }
    }

    float seconds() {
        using clock = std::chrono::steady_clock;
        static const clock::time_point start = clock::now();
        return std::chrono::duration<float>(clock::now() - start).count();
    }
}

void rush2::fancy_ufo::init_config() {
    recomp::config::Config& config = rush2::wings::games_config();
    config.add_bool_option(
        option_id,
        "Fancy UFO",
        "Dresses up the PlayStation UFO. Its ENGINE and HORN rows on the car select get a starship's sounds, WARP CORE and "
        "HAILING, which it starts on. A beam of light shines down from each of its three pods. In the air, while the wings "
        "button holds it up, thrusters on its rim fire the way you steer it (needs the Rush 2049 ROM and Wings on, as wings "
        "do). The lights and thrusters change at once; the sounds from the next race.",
        true
    );
    config.add_option_change_callback(option_id,
        [](recomp::config::ConfigValueVariant cur_value, recomp::config::ConfigValueVariant, recomp::config::OptionChangeContext) {
            option = std::get<bool>(cur_value);
        });
    // The sounds, on a worker (a race with the UFO may use them whatever the option is now).
    std::thread([] { make_sounds(); }).detach();
}

void rush2::fancy_ufo::add_option(rush2::ui::OptionsPage* page) {
    page->add_option(rush2::wings::games_config(), option_id);
}

bool rush2::fancy_ufo::enabled() {
    return option.load(std::memory_order_relaxed);
}

bool rush2::fancy_ufo::is_ufo(int type) {
    if (!ct::is_mod(type) || !rush2::car2049::mod_car_from_sfrush(type)) return false;
    return rush2::car2049::mod_car_name(type - ct::first_mod) == "UFO";
}

const std::vector<int16_t>& rush2::fancy_ufo::engine_hum() {
    return ready_or_none(hum);
}

const std::vector<int16_t>& rush2::fancy_ufo::engine_whine() {
    return ready_or_none(whine);
}

const std::vector<int16_t>& rush2::fancy_ufo::horn() {
    return ready_or_none(horn_pcm);
}

void rush2::fancy_ufo::engine_mix(float drive, Layer out[2]) {
    float d = std::clamp(drive, 0.0f, 1.0f);
    out[0] = { 0.9f + 0.45f * d, 0.8f + 0.2f * d };
    out[1] = { 0.7f + 0.9f * d, 0.1f + 0.6f * d };
}

void rush2::fancy_ufo::thruster(int i, float out[3]) {
    // The rim's edge: 52 out (its widest is 53, 1/16 ft) and 14.4 up (its rim plate) on the UFO at Rush 2's width
    // cap, at its size now (in feet).
    constexpr float r = 52.0f * grow / 16.0f, y = up_scaled(14.4f) / 16.0f;
    constexpr float at[thrusters][3] = { { r, y, 0.0f }, { -r, y, 0.0f }, { 0.0f, y, r }, { 0.0f, y, -r } };
    for (int k = 0; k < 3; k++) out[k] = at[std::clamp(i, 0, thrusters - 1)][k];
}

int rush2::fancy_ufo::engine_step(uint8_t* rdram, recomp_context* ctx, int before) {
    int type = (int)(int8_t)ctx->r7;
    if (before < 0 || !enabled() || !is_ufo(type)) return -1;
    constexpr int values = engine_value + 1;
    int step = (int64_t)ctx->r5 > 0 ? 1 : -1;
    int locked = (int)(int32_t)ctx->r18;   // Rush 2's loop skips this one ($s2)
    int value = std::clamp(before, 0, engine_value);
    for (int tries = 0; tries < values; tries++) {
        value = (value + step + values) % values;
        if (value != locked) break;
    }
    return value;
}

uint32_t rush2::fancy_ufo::label(uint8_t* rdram, int type, bool horn_row, int value) {
    if (horn_row ? value != horn_value : value != engine_value) return 0;
    if (!is_ufo(type)) return 0;
    uint32_t at = horn_row ? horn_label : engine_label;
    put_label(rdram, at, horn_row ? "HAILING" : "WARP CORE");
    return at;
}

void rush2::fancy_ufo::forget_record(uint8_t* rdram, uint32_t record) {
    std::lock_guard lock{ store_mutex };
    load_store();
    if (std::erase_if(picks, [record](const auto& e) { return e.first.first >= record && e.first.first < record + block_span; }) != 0) {
        save_store();
    }
}

// A getter's value (*value) of `field` for car type `type`, the record's byte at `at`: the UFO's own sound while the
// option is on and the record holds the stand-in for it (or nothing was picked yet).
extern "C" void rush2_fancy_ufo_get(uint8_t* rdram, recomp_context* ctx, uint32_t at, int type, int field, uint64_t* value) {
    if (!rush2::fancy_ufo::enabled() || !rush2::fancy_ufo::is_ufo(type)) return;
    uint32_t key = rush2::car2049::record_address(rdram, at);
    std::lock_guard lock{ store_mutex };
    load_store();
    auto it = picks.find({ key, field });
    if (it == picks.end() || (it->second == 1 && (int)(*value & 0xFF) == stand_in(rdram, field, type))) {
        *value = (uint64_t)own_value(field);
    }
}

// A setter before it stores (the record's byte at `at`, the value in $a2): the UFO's own sound is kept beside the
// record, which gets the type's default.
extern "C" void rush2_fancy_ufo_set(uint8_t* rdram, recomp_context* ctx, uint32_t at, int type, int field) {
    if (!rush2::fancy_ufo::is_ufo(type)) return;
    int value = (int)(ctx->r6 & 0xFF);
    int own = value >= own_value(field) ? 1 : 0;
    if (own) ctx->r6 = (uint64_t)stand_in(rdram, field, type);
    uint32_t key = rush2::car2049::record_address(rdram, at);
    std::lock_guard lock{ store_mutex };
    load_store();
    auto it = picks.find({ key, field });
    if (it == picks.end() || it->second != own) {
        picks[{ key, field }] = own;
        save_store();
    }
}

// Car select HORN row (0x803B9D5C): the row's byte before the step is in $t7.
extern "C" void rush2_fancy_ufo_horn_before(uint8_t* rdram, recomp_context* ctx) {
    horn_before = (int)(int8_t)ctx->r15;
}

// Car select HORN row at 0x803B9DD0, after Rush 2's step (or reset, $a1 0) and wrap at 9: $v0 + 0x79 the row's byte,
// $v1 its value, $a0 0x800C0000 + value (the horn's sound id at +0x1000 is played next), the type at $t1. On the UFO
// the row steps through HAILING too, which a reset picks, and HAILING plays here.
extern "C" void rush2_fancy_ufo_horn_value(uint8_t* rdram, recomp_context* ctx) {
    int before = horn_before;
    horn_before = -1;
    int type = (int)(int8_t)MEM_B(0, (int32_t)ctx->r9);
    if (!rush2::fancy_ufo::enabled() || !rush2::fancy_ufo::is_ufo(type)) return;
    constexpr int values = rush2::fancy_ufo::horn_value + 1;
    int step = (int64_t)ctx->r5 > 0 ? 1 : (int64_t)ctx->r5 < 0 ? -1 : 0;
    int value = rush2::fancy_ufo::horn_value;
    if (step != 0 && before >= 0) value = (std::clamp(before, 0, values - 1) + step + values) % values;
    MEM_B(0x79, (int32_t)ctx->r2) = (int8_t)value;
    ctx->r3 = (uint64_t)value;
    ctx->r4 = (uint64_t)(int64_t)(int32_t)(0x800C0000u + uint32_t(std::min(value, values - 2)));
    if (value == rush2::fancy_ufo::horn_value) {
        play_horn(rdram, menu_hold);
        skip_next_sound = true;
    }
}

// car_sound_update_all at 0x80067558, as a player's horn sounds (when the button is held, again every so often):
// $t7 = the race's HORN value of the car. Rush 2's sound for HAILING (past its table) is skipped; HAILING itself is
// sounded by rush2_fancy_ufo_horn_held below.
extern "C" void rush2_fancy_ufo_race_horn(uint8_t* rdram, recomp_context* ctx) {
    static const bool log_on = std::getenv("RUSH2_UFO_LOG") != nullptr;
    if (log_on) std::printf("[fancy_ufo] race horn, HORN %d\n", (int)(int8_t)ctx->r15);
    if ((int)(int8_t)ctx->r15 >= rush2::fancy_ufo::horn_value) skip_next_sound = true;
}

// car_sound_update_all at 0x800674F0, every physics tick per player: $v0 the player's record (0x800C2140 + 0x28 x
// player: +0x1C the horn button's mask, +2 the buttons pressed this tick, which Rush 2 tests here in $t1, +4 those
// held), $s1 the car, $s2 the player (the race's HORN value is 0x80205B19 [player x 121 + the car's type at +0x7EA],
// as read at 0x8006754C). HAILING sounds on while the button stays held (whatever the option now: the race keeps the
// horn it started with), and fades out once it's let go.
extern "C" void rush2_fancy_ufo_horn_held(uint8_t* rdram, recomp_context* ctx) {
    if ((MEM_HU(0x4, (int32_t)ctx->r2) & MEM_HU(0x1C, (int32_t)ctx->r2)) == 0) return;
    const uint32_t horn_rows = ct::table(ct::horn) + ct::types;   // 0x80205B19: player 1's row
    uint32_t type = MEM_BU(0x7EA, (int32_t)ctx->r17);
    uint32_t player = (uint32_t)ctx->r18;
    if (player > 7 || type >= ct::types) return;
    if ((int8_t)MEM_B(0, (int32_t)(horn_rows + player * ct::types + type)) >= rush2::fancy_ufo::horn_value) {
        play_horn(rdram, tick_hold);
    }
}

// menu_play_sound entry: true for the Rush 2 horn a HAILING above replaced.
extern "C" int rush2_fancy_ufo_sound_skip(uint8_t* rdram, recomp_context* ctx) {
    return skip_next_sound.exchange(false) ? 1 : 0;
}

// Car select ENGINE row at 0x803B9EBC, before the value is saved: a reset ($a1 0) of the UFO's row picks WARP CORE.
// $v0 = the value, $a3 the type, $s7 the player.
extern "C" void rush2_fancy_ufo_engine_reset(uint8_t* rdram, recomp_context* ctx) {
    int type = (int)(int8_t)ctx->r7;
    int player = (int)ctx->r23;
    if (ctx->r5 != 0 || player < 0 || player > 1 || !rush2::fancy_ufo::enabled() || !rush2::fancy_ufo::is_ufo(type)) return;
    MEM_B(0, (int32_t)(ct::table(ct::engine) + uint32_t((player + 1) * ct::types + type))) = (int8_t)rush2::fancy_ufo::engine_value;
    ctx->r2 = (uint64_t)rush2::fancy_ufo::engine_value;
}

// pvs_apply 0x8007C27C, before each view: the hover glow of every race car that is the UFO.
extern "C" void rush2_fancy_ufo_view(uint8_t* rdram, recomp_context* ctx) {
    (void)ctx;
    bool racing = (int32_t)MEM_W(0, (int32_t)game_state) >= 3;
    if (!models_written) {
        write_beam(rdram);
        write_ring(rdram);
        write_jet(rdram);
        rush2::battle_render::add_model(beam_model, beam_dl);
        rush2::battle_render::add_model(ring_model, ring_dl);
        rush2::battle_render::add_model(jet_model, jet_dl);
        models_written = true;
    }
    const float now = seconds();
    for (int i = 0; i < max_cars; i++) {
        const uint32_t car = cars + uint32_t(i) * car_size;
        int type = MEM_BU(0, (int32_t)(car + car_type));
        if (!racing || !rush2::fancy_ufo::enabled() || MEM_H(0, (int32_t)(car + car_active)) == 0 ||
            MEM_B(0, (int32_t)(car + car_wrecked)) != 0 || (MEM_W(0, (int32_t)(car + car_flags)) & 0x10) != 0 ||
            rush2::ghost::is_ghost_car(i) || !rush2::fancy_ufo::is_ufo(type)) {
            hide_car(i);
            continue;
        }
        float pos[3], m[9];
        rush2::battle::car_body_pose(rdram, i, pos, m);
        // The beams shimmer: two waves of brightness, offset per car.
        float t = now + float(i) * 0.37f;
        float glow = 0.72f + 0.14f * std::sin(2.0f * pi * 3.1f * t) + 0.08f * std::sin(2.0f * pi * 7.3f * t + 1.0f);
        uint32_t alpha = uint32_t(std::clamp(glow, 0.0f, 1.0f) * 255.0f);
        rush2::battle_render::place(first_slot + i, beam_model, m, pos, glow_rgb | alpha);
        // The rings: each pod's run down its beam every 0.6 s, two at a time half a run apart, growing and fading.
        constexpr float period = 0.6f;
        for (int p = 0; p < 3; p++) {
            for (int k = 0; k < rings_per_pod; k++) {
                float phase = std::fmod(t / period + float(k) / float(rings_per_pod) + float(p) * 0.21f, 1.0f);
                float y = pod_tip - 14.0f * grow * phase;
                float scale = (0.35f + 0.85f * phase) * grow;
                float local[3] = { pods[p][0] / 16.0f, y / 16.0f, pods[p][1] / 16.0f };
                float at[3], rm[9];
                for (int c = 0; c < 3; c++) {
                    at[c] = pos[c] + m[c] * local[0] + m[3 + c] * local[1] + m[6 + c] * local[2];
                    rm[c] = m[c] * scale;
                    rm[3 + c] = m[3 + c] * grow;
                    rm[6 + c] = m[6 + c] * scale;
                }
                uint32_t ring_alpha = uint32_t(std::sin(pi * phase) * 0.8f * 255.0f);
                rush2::battle_render::place(ring_slot + (i * 3 + p) * rings_per_pod + k, ring_model, rm, at, glow_rgb | ring_alpha);
            }
        }
        place_jets(rdram, i, pos, m, now);
        shown[i] = true;
    }
}
