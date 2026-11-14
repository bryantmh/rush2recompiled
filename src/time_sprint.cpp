// CHECKPOINTS' third value, CLOCKS: the SF Rush PlayStation version's Time Sprint mode (docs/sfrush_ps1.md#time-sprint).
//
// The PS1 (Time Sprint is gp+0x3C, set by its mode menu func_8001A0D8 at 0x8001A730 with 3 laps): props_place
// (func_800155F8, 0x80015950-0x80015A8C) puts the clocks of the raced track (RUSH.EXE 0x800A59C4) in the prop array as
// kind 8, the model EXTRA/TIME.PSV (an hourglass 5 ft across and 5.6 ft tall, its origin its middle, loaded by
// func_80036C40 at 0x8003784C, scale 0x140). props_update (func_80041244) draws a clock (0x80041484) with func_80046DD4:
// texture page 13 (gp+0x2F8: VRAM 832, 0) at v + 0x40 (gp+0x2FC) and CLUT id 0x1037 (VRAM 880, 64, plus the distance
// fog row), which is ART/PICKUPS.GFX: func_8003CF20 uploads it to (832, 64, 64, 64) with its CLUT at (880, 64), and the
// hourglass uses its right half (u 64-127; the key the left). It doesn't turn (the prop's rotation stays 0). A clock a
// car reaches (0x80041538) goes (state 0: for the rest of the race), adds 20 s to the clock gp+0x144 (24 at
// difficulty 5, 27 at 6) and plays sound 3 of the race's bank (BANKS/GAMESND.BNK: CHECK, centered, pitch 0x8CA).
// checkpoint_passed (func_80054130) returns at once in this mode, so the gates add nothing.
//
// Here: the clocks of the raced version (N64, arcade or the PS1 mod's) from src/sfrush_ps1/time_sprint_clocks.inc,
// drawn by rush2::battle_render (slots 202-223) with the PS1's own model and texture and taken by any human player's
// car within reach. Taking one plays the PS1's CHECK sound (rush2::car_engines::play_sample) and adds 20 s, scaled by
// difficulty as Rush 2 scales a checkpoint's time (as the PS1 courses' checkpoint times are, in place of the PS1's
// difficulty bonus), to the time allowed (race_time_float 0x8010C204), raising Rush 2's BONUS TIME banner as a
// checkpoint does (bonus_time_banner_end 0x8010C3C4). checkpoint_passed_8008F220 skips its time bonus (hook at
// 0x8008F664). The model, texture and sound come from the PS1 disc through the sfrush_ps1 mod (ps1_cars.cpp build_mod
// writes them), read once on a worker thread: without the mod there is no Time Sprint. The race's laps are the menu's;
// Rush 2's start time is kept. The taken flags are race state, so online they are part of the resync
// (rush2::time_sprint::online_sync).

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "recomp.h"
#include "librecomp/mods.hpp"

#include "battle_render.h"
#include "checkpoint_time.h"
#include "car_engines.h"
#include "online_sync.h"
#include "rush2_hooks.h"
#include "sfrush_ps1.h"
#include "texture_upscale.h"
#include "time_sprint.h"
#include "track1.h"
#include "track_mod_runtime.h"

namespace {
    struct Row {
        const char* mod_id;     // "" = the N64 course
        const char* folder;
        int course;             // SF Rush 0-2, Chinatown 17
        int index;              // the PS1 clock's
        float pos[3];           // its middle, Rush 2 world ft
    };
    const Row rows[] = {
#include "sfrush_ps1/time_sprint_clocks.inc"
    };

    constexpr uint32_t checkpoints = 0x8010C17B;     // u8: CHECKPOINTS as the race runs it (0 off, 1 on, 2 CLOCKS)
    constexpr uint32_t time_allowed = 0x8010C204;    // f32: race_time_float, the clock runs out when the game clock passes it
    constexpr uint32_t banner_end = 0x8010C3C4;      // f32: bonus_time_banner_end
    constexpr uint32_t game_clock = 0x80117488;      // f32: timer_game_time
    constexpr uint32_t physics_clock = 0x8010C0E4;   // f32: the race's physics time
    constexpr uint32_t attract_demo = 0x800FAE6C;    // u8
    constexpr uint32_t race_cars = 0x8010C15A;       // s8
    constexpr uint32_t cars = 0x800F5470, car_size = 0x81C;
    constexpr uint32_t car_kind = 0x7E8;             // u8: 2 human
    constexpr uint32_t car_position = 0x224;         // f32[3] world
    constexpr uint32_t car_wrecked = 0x648;          // s8
    constexpr uint32_t car_states = 0x801124A0, car_state_size = 0x354;
    constexpr uint32_t state_finished = 0xEA;        // u8
    constexpr uint32_t state_out = 0x343;            // u8
    constexpr int max_cars = 8;

    constexpr int first_slot = 202;                  // to 223 (include/battle_render.h)
    constexpr int max_clocks = 22;
    constexpr const char* model_name = "TIME_SPRINT_CLOCK";
    // Spare RDRAM (the list in src/controls_menu.cpp): the hourglass's texels, CLUT, vertices and display list.
    constexpr uint32_t texels_at = 0x80E14040;       // 64 x 64 CI4, 2048 bytes
    constexpr uint32_t tlut_at = 0x80E14840;         // 16 x RGBA5551
    constexpr uint32_t vertices_at = 0x80E14860;     // 13 faces x 4 corners x 16 bytes, to 0x80E14BA0
    constexpr uint32_t dl_at = 0x80E14BA0;
    constexpr uint32_t dl_end = 0x80E14FA0;
    constexpr float reach_across = 8.0f;             // ft from the clock's middle to the car's origin, across
    constexpr float reach_up = 10.0f;                // and up or down
    constexpr uint32_t ps1_rate = 0x8CA * 44100 / 0x1000;   // the CHECK sound's pitch (SPU 0x1000 = 44.1 kHz): 24224 Hz

    struct Clock {
        float pos[3];
        bool taken = false;
    };

    std::mutex state_mutex;
    std::vector<Clock> clocks;      // the raced version's
    bool sprint = false;            // this race is a Time Sprint
    bool decided = false;           // whether this race's clocks were looked up (once a race)
    float last_physics = 0.0f;
    bool model_written = false;     // the clock's model is in RDRAM for this race
    bool model_added = false;
    bool shown = false;

    float read_f(uint8_t* rdram, uint32_t at) {
        uint32_t w = (uint32_t)MEM_W(0, (int32_t)at);
        float f;
        std::memcpy(&f, &w, 4);
        return f;
    }

    void write_f(uint8_t* rdram, uint32_t at, float f) {
        uint32_t w;
        std::memcpy(&w, &f, 4);
        MEM_W(0, (int32_t)at) = (int32_t)w;
    }

    // The PS1's clock as the sfrush_ps1 mod carries it, made ready for the N64 on a worker.
    struct Assets {
        std::array<uint8_t, 2048> texels{};           // u 64-127, v 0-63 of PICKUPS.GFX, CI4, high nibble first
        std::array<uint16_t, 16> tlut{};              // its CLUT as RGBA5551 (0x0000 transparent)
        std::vector<std::array<int16_t, 5>> verts;    // per face corner: x, y, z (1/16 ft), s, t (10.5 texels)
        std::vector<int16_t> pcm;                     // CHECK, mono at ps1_rate
    };
    std::mutex assets_mutex;
    std::shared_ptr<const Assets> assets;            // set once loaded
    bool loading = false;
    bool load_failed = false;
    bool failed_logged = false;
    std::chrono::steady_clock::time_point failed_at;

    // PS1 SPU ADPCM: 16-byte blocks of shift and filter, flags, 28 4-bit samples (low nibble first); flag bit 0 ends
    // the sound.
    std::vector<int16_t> decode_adpcm(const std::vector<uint8_t>& in) {
        static const int f0[5] = { 0, 60, 115, 98, 122 }, f1[5] = { 0, 0, -52, -55, -60 };
        std::vector<int16_t> out;
        int s1 = 0, s2 = 0;
        for (size_t b = 0; b + 16 <= in.size(); b += 16) {
            int shift = in[b] & 0xF, filter = std::min(in[b] >> 4, 4), flags = in[b + 1];
            for (int k = 0; k < 28; k++) {
                int nib = (in[b + 2 + k / 2] >> ((k & 1) * 4)) & 0xF;
                int v = int(int16_t(uint16_t(nib << 12))) >> shift;
                v += (s1 * f0[filter] + s2 * f1[filter] + 32) / 64;
                v = std::clamp(v, -32768, 32767);
                s2 = s1;
                s1 = v;
                out.push_back(int16_t(v));
            }
            if (flags & 1) break;
        }
        return out;
    }

    std::shared_ptr<Assets> load_assets() {
        std::filesystem::path path = recomp::mods::get_mod_filename(rush2::sfrush_ps1::mod_id);
        if (path.empty()) return nullptr;
        std::vector<uint8_t> psv, page, sound;
        std::unique_ptr<rush2::upscale::ZipReader> zip;
        if (!std::filesystem::is_directory(path)) zip = std::make_unique<rush2::upscale::ZipReader>(path);
        auto read = [&](const char* name, std::vector<uint8_t>& out) {
            if (zip == nullptr) {
                out = rush2::upscale::read_file(path / name);
                return !out.empty();
            }
            return zip->is_open() && zip->read(name, out);
        };
        if (!read("rush2/time_sprint/time.psv", psv) || !read("rush2/time_sprint/pickups.gfx", page) ||
            !read("rush2/time_sprint/check.adpcm", sound) || page.size() < 0x28 + 128 * 64) {
            return nullptr;
        }
        std::vector<std::array<double, 3>> pv;
        std::vector<rush2::sfrush_ps1::PsvFace> faces;
        if (!rush2::sfrush_ps1::read_psv(psv, pv, faces)) return nullptr;
        auto a = std::make_shared<Assets>();
        // The page: an 8-byte header, its CLUT (16 x PS1 15-bit), then 256 x 64 4-bit texels.
        for (int c = 0; c < 16; c++) {
            uint16_t v = uint16_t(page[size_t(8 + c * 2)] | page[size_t(9 + c * 2)] << 8);
            uint16_t r = v & 31, g = (v >> 5) & 31, b = (v >> 10) & 31;
            a->tlut[size_t(c)] = uint16_t(r << 11 | g << 6 | b << 1 | (v != 0 ? 1 : 0));
        }
        for (int v = 0; v < 64; v++) {
            for (int u = 0; u < 64; u += 2) {
                uint8_t ps1 = page[size_t(0x28 + v * 128 + (64 + u) / 2)];
                a->texels[size_t(v * 32 + u / 2)] = uint8_t((ps1 & 0xF) << 4 | ps1 >> 4);
            }
        }
        // Corners: the PSV's coordinates x 0x140 / 640 ft (its load scale, as ps1_track.cpp's props) in the converters'
        // game frame (PS1 y, z, x; z is up), texels inset as the converters' (texel_inset), in the 64-texel image.
        constexpr double k = 0x140 / 640.0 * 16.0;
        for (const rush2::sfrush_ps1::PsvFace& f : faces) {
            if (f.idx.size() != 4 || f.uv.size() != 4) continue;
            std::array<double, 2> centre{};
            for (const auto& t : f.uv) centre[0] += t[0] / 4.0, centre[1] += t[1] / 4.0;
            for (size_t c = 0; c < 4; c++) {
                if (f.idx[c] >= pv.size()) return nullptr;
                const auto& p = pv[f.idx[c]];
                std::array<double, 2> t = rush2::sfrush_ps1::texel_inset(f.uv[c], centre);
                a->verts.push_back({ int16_t(std::lround(p[1] * k)), int16_t(std::lround(p[2] * k)),
                                     int16_t(std::lround(p[0] * k)), int16_t(std::lround((t[0] - 64.0) * 32.0)),
                                     int16_t(std::lround(t[1] * 32.0)) });
            }
        }
        a->pcm = decode_adpcm(sound);
        if (a->verts.empty() || a->pcm.empty()) return nullptr;
        return a;
    }

    std::shared_ptr<const Assets> loaded_assets() {
        std::lock_guard lock{ assets_mutex };
        return assets;
    }

    // The clock's texels, CLUT, vertices and display list into RDRAM: TIME.PSV with PICKUPS.GFX, CI4 with its 16-color
    // CLUT as the PS1 draws it. Each face is drawn both ways round, so it shows whatever culling the view left on. No
    // fog, as the checkpoint banners (src/arcade2049/arcade_checkpoints.cpp): the fog in place at the end of the view
    // isn't the scenery's.
    bool write_model(uint8_t* rdram, const Assets& a) {
        for (size_t i = 0; i < a.texels.size(); i++) MEM_B(0, (int32_t)(texels_at + i)) = (int8_t)a.texels[i];
        for (size_t i = 0; i < a.tlut.size(); i++) MEM_H(0, (int32_t)(tlut_at + i * 2)) = (int16_t)a.tlut[i];
        size_t corners = std::min<size_t>(a.verts.size(), (dl_at - vertices_at) / 16) / 4 * 4;
        for (size_t i = 0; i < corners; i++) {
            uint32_t at = vertices_at + uint32_t(i) * 16;
            const auto& v = a.verts[i];
            MEM_W(0, (int32_t)at) = (int32_t)((uint32_t(uint16_t(v[0])) << 16) | uint16_t(v[1]));
            MEM_W(4, (int32_t)at) = (int32_t)(uint32_t(uint16_t(v[2])) << 16);
            MEM_W(8, (int32_t)at) = (int32_t)((uint32_t(uint16_t(v[3])) << 16) | uint16_t(v[4]));
            MEM_W(12, (int32_t)at) = (int32_t)0xFFFFFFFFu;
        }
        std::vector<std::array<uint32_t, 2>> dl = {
            { 0xE7000000, 0x00000000 },        // G_RDPPIPESYNC
            { 0xE3001001, 0x00008000 },        // TEXTLUT = RGBA16
            { 0xE3000F00, 0x00000000 },        // TEXTLOD = TILE
            { 0xE200001C, 0x0C192230 },        // render mode: no fog, then z-buffered opaque surface
            { 0xFC127FFF, 0xFFFFF238 },        // combiner: texel 0 x shade
            // The CLUT into TMEM 256 (through tile 7), then the texels (64 x 64 CI4 loaded as 16-bit, 1024 words).
            { 0xFD100000, tlut_at },           // G_SETTIMG RGBA16
            { 0xE8000000, 0x00000000 },        // G_RDPTILESYNC
            { 0xF5000100, 0x07000000 },        // tile 7 at TMEM 256
            { 0xE6000000, 0x00000000 },        // G_RDPLOADSYNC
            { 0xF0000000, 0x0703C000 },        // G_LOADTLUT 16 colors
            { 0xE7000000, 0x00000000 },
            { 0xFD500000, texels_at },         // G_SETTIMG CI 16-bit
            { 0xF5500000, 0x07000000 },        // load tile 7
            { 0xE6000000, 0x00000000 },
            { 0xF3000000, 0x073FF200 },        // G_LOADBLOCK 1024 words, dxt 512 (a 64-texel 4-bit row is 4 words)
            { 0xE7000000, 0x00000000 },
            { 0xF5400800, 0x00018060 },        // tile 0: CI4, 4 words a row, palette 0, wrapped, 64 x 64
            { 0xF2000000, 0x000FC0FC },        // tile size 64 x 64
        };
        const size_t batch = 7;   // faces (28 corners) a G_VTX
        size_t faces = corners / 4;
        for (size_t first = 0; first < faces; first += batch) {
            size_t count = std::min(batch, faces - first);
            uint32_t verts = uint32_t(count * 4);
            dl.push_back({ 0x01000000u | (verts << 12) | (verts << 1), vertices_at + uint32_t(first * 4) * 16 });
            for (size_t q = 0; q < count; q++) {
                uint32_t v = uint32_t(q * 4) * 2;
                dl.push_back({ 0x06000000u | (v << 16) | ((v + 2) << 8) | (v + 4), (v << 16) | ((v + 4) << 8) | (v + 6) });
                dl.push_back({ 0x06000000u | (v << 16) | ((v + 4) << 8) | (v + 2), (v << 16) | ((v + 6) << 8) | (v + 4) });
            }
        }
        dl.push_back({ 0xE7000000, 0x00000000 });
        dl.push_back({ 0xE3001001, 0x00000000 });   // TEXTLUT = NONE for the models drawn after it
        dl.push_back({ 0xDF000000, 0x00000000 });   // G_ENDDL
        if (dl_at + dl.size() * 8 > dl_end) return false;
        for (size_t i = 0; i < dl.size(); i++) {
            MEM_W(0, (int32_t)(dl_at + i * 8)) = (int32_t)dl[i][0];
            MEM_W(4, (int32_t)(dl_at + i * 8)) = (int32_t)dl[i][1];
        }
        return true;
    }

    // Rush 2's checkpoint time factor (checkpoint_passed_8008F220 at 0x8008F640-0x8008F680): 1 + (5 - difficulty) x
    // the step at 0x800CFBEC, with the Checkpoint Time Difficulty setting's difficulty (src/checkpoint_time.cpp).
    float difficulty_factor(uint8_t* rdram) {
        constexpr uint32_t step = 0x800CFBEC;         // f32
        return 1.0f + float(5 - rush2::checkpoint_time::difficulty(rdram)) * read_f(rdram, step);
    }

    void hide_all() {
        if (!shown) return;
        for (int i = 0; i < max_clocks; i++) rush2::battle_render::hide(first_slot + i);
        shown = false;
    }

    // The raced version's clocks: a custom track's by its mod and folder, else the N64 SF Rush course's.
    std::vector<Clock> raced_clocks() {
        std::vector<Clock> out;
        int mod = rush2::track_mod::race_track();
        std::string mod_id, folder;
        int course = -1;
        if (mod >= 0) {
            std::vector<rush2::track_mod::Entry> list = rush2::track_mod::tracks();
            if (mod >= (int)list.size()) return out;
            mod_id = list[size_t(mod)].mod_id;
            folder = list[size_t(mod)].folder;
        }
        else {
            int k = rush2::track1::race_track();
            if (k <= 0) return out;
            course = k - 1;
        }
        for (const Row& r : rows) {
            bool match = mod >= 0 ? (mod_id == r.mod_id && folder == r.folder) : (r.mod_id[0] == 0 && r.course == course);
            if (!match || (int)out.size() >= max_clocks) continue;
            Clock c;
            std::memcpy(c.pos, r.pos, sizeof(c.pos));
            out.push_back(c);
        }
        return out;
    }
}

bool rush2::time_sprint::has_clocks_n64(int course) {
    for (const Row& r : rows) {
        if (r.mod_id[0] == 0 && r.course == course) return true;
    }
    return false;
}

bool rush2::time_sprint::has_clocks(const std::string& mod_id, const std::string& folder) {
    for (const Row& r : rows) {
        if (mod_id == r.mod_id && folder == r.folder) return true;
    }
    return false;
}

bool rush2::time_sprint::available() {
    if (!recomp::mods::is_mod_enabled(rush2::sfrush_ps1::mod_id)) return false;
    std::lock_guard lock{ assets_mutex };
    if (assets != nullptr) return true;
    // A mod from before Time Sprint is rebuilt at launch (its key holds the build): look again now and then.
    auto now = std::chrono::steady_clock::now();
    if (load_failed && now - failed_at < std::chrono::seconds(5)) return false;
    if (!loading) {
        loading = true;
        std::thread([]() {
            std::shared_ptr<Assets> a = load_assets();
            std::lock_guard lock{ assets_mutex };
            assets = a;
            loading = false;
            load_failed = a == nullptr;
            failed_at = std::chrono::steady_clock::now();
            if (a != nullptr || !failed_logged) {
                failed_logged = true;
                std::printf("[time_sprint] %s\n", a != nullptr ? "the PS1 clock is loaded"
                                                              : "the PS1 clock couldn't be read from the sfrush_ps1 mod");
                std::fflush(stdout);
            }
        }).detach();
    }
    return !load_failed;   // while loading after a failure, still no
}

bool rush2::time_sprint::on(uint8_t* rdram) {
    (void)rdram;
    std::lock_guard lock{ state_mutex };
    return sprint;
}

namespace rush2::time_sprint {
    void online_sync(rush2::online::sync::Walker& w) {
        std::lock_guard lock{ state_mutex };
        rush2::online::sync::vector(w, clocks, [](rush2::online::sync::Walker& w, Clock& c) {
            rush2::online::sync::field(w, c.pos);
            rush2::online::sync::field(w, c.taken);
        });
        rush2::online::sync::field(w, sprint);
        rush2::online::sync::field(w, decided);
    }
}

// physics_tick_80076578 at 0x8007660C, the end of a physics tick: the clocks placed and taken.
extern "C" void rush2_time_sprint_tick(uint8_t* rdram, recomp_context* ctx) {
    (void)ctx;
    std::lock_guard lock{ state_mutex };
    float now = read_f(rdram, physics_clock);
    if (now < last_physics) decided = false;   // a new race (or a restart)
    last_physics = now;
    if (!decided) {
        decided = true;
        sprint = false;
        model_written = false;
        clocks.clear();
        hide_all();
        if (MEM_BU(0, (int32_t)checkpoints) == rush2::time_sprint::clocks_value && MEM_BU(0, (int32_t)attract_demo) == 0 &&
            rush2::time_sprint::available()) {
            clocks = raced_clocks();
            // Test aid: RUSH2_TEST_TIME_SPRINT_AHEAD=<ft> puts one more clock that far in front of player 1's car.
            if (const char* ahead = std::getenv("RUSH2_TEST_TIME_SPRINT_AHEAD"); ahead != nullptr && !clocks.empty()) {
                constexpr uint32_t car_matrix = 0x2CC;   // f32[9]: rows right, up, forward
                Clock c;
                for (int k = 0; k < 3; k++) {
                    c.pos[k] = read_f(rdram, cars + car_position + uint32_t(k) * 4) +
                               read_f(rdram, cars + car_matrix + 24 + uint32_t(k) * 4) * (float)std::atof(ahead);
                }
                c.pos[1] += 2.0f;
                clocks.insert(clocks.begin(), c);
                if (clocks.size() > size_t(max_clocks)) clocks.pop_back();
            }
            sprint = !clocks.empty();
            if (sprint) {
                std::printf("[time_sprint] %zu clocks on this track\n", clocks.size());
                std::fflush(stdout);
            }
        }
    }
    if (!sprint) {
        hide_all();
        return;
    }
    std::shared_ptr<const Assets> a = loaded_assets();
    if (a != nullptr && !model_written) {
        model_written = write_model(rdram, *a);
        if (model_written && !model_added) {
            rush2::battle_render::add_model(model_name, dl_at);
            model_added = true;
        }
    }
    // Taken by a human player's car within reach (each clock once a race).
    int n = std::min(max_cars, std::max(0, (int)(int8_t)MEM_B(0, (int32_t)race_cars)));
    for (int c = 0; c < n; c++) {
        uint32_t car = cars + (uint32_t)c * car_size;
        uint32_t state = car_states + (uint32_t)c * car_state_size;
        // Test aid: RUSH2_TEST_TIME_SPRINT_DRONES=1 lets the drones take clocks too, so a run watching a drone drive
        // the AI lanes (tools/test_race.py --drones 1 --env RUSH2_TEST_RACE_VIEW_CAR=1) shows them taken.
        static const bool test_drones = std::getenv("RUSH2_TEST_TIME_SPRINT_DRONES") != nullptr;
        uint8_t kind = MEM_BU(0, (int32_t)(car + car_kind));
        if ((kind != 2 && !(test_drones && kind == 1)) || MEM_B(0, (int32_t)(car + car_wrecked)) != 0) continue;
        if (MEM_BU(0, (int32_t)(state + state_finished)) != 0 || MEM_BU(0, (int32_t)(state + state_out)) != 0) continue;
        float p[3];
        for (int k = 0; k < 3; k++) p[k] = read_f(rdram, car + car_position + uint32_t(k) * 4);
        // Test aid: RUSH2_TEST_TIME_SPRINT_REACH=<ft> widens the reach across, for a drone on its lane.
        static const float reach = std::getenv("RUSH2_TEST_TIME_SPRINT_REACH") != nullptr
                                       ? (float)std::atof(std::getenv("RUSH2_TEST_TIME_SPRINT_REACH")) : reach_across;
        for (Clock& clock : clocks) {
            if (clock.taken) continue;
            if (std::hypot(p[0] - clock.pos[0], p[2] - clock.pos[2]) > reach ||
                std::fabs(p[1] - clock.pos[1]) > reach_up) {
                continue;
            }
            clock.taken = true;
            float added = rush2::time_sprint::clock_seconds * difficulty_factor(rdram);
            write_f(rdram, time_allowed, read_f(rdram, time_allowed) + added);
            write_f(rdram, banner_end, read_f(rdram, game_clock) + 3.0f);   // as checkpoint_passed's 0x8008F6A0
            if (a != nullptr) rush2::car_engines::play_sample(rdram, "ps1_check", a->pcm, ps1_rate);
            std::printf("[time_sprint] car %d took a clock (+%.1f s)\n", c, added);
            std::fflush(stdout);
        }
    }
    // Shown standing still, as the PS1's, the taken ones hidden.
    static const float m[9] = { 1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f };
    for (size_t i = 0; i < size_t(max_clocks); i++) {
        if (model_written && i < clocks.size() && !clocks[i].taken) {
            rush2::battle_render::place(first_slot + int(i), model_name, m, clocks[i].pos, 0xFFFFFFFF);
        }
        else {
            rush2::battle_render::hide(first_slot + int(i));
        }
    }
    shown = true;
}

// checkpoint_passed_8008F220 at 0x8008F664, about to add the checkpoint's time to the time allowed and raise the
// BONUS TIME banner (to 0x8008F6A0). Returns 1 to skip both in a Time Sprint (the clocks give the time).
extern "C" int rush2_time_sprint_checkpoint(uint8_t* rdram, recomp_context* ctx) {
    (void)ctx;
    return rush2::time_sprint::on(rdram) ? 1 : 0;
}

// checkpoint_passed_8008F220 at 0x8008F418, about to play a human car's checkpoint sounds (0x8008F480-0x8008F4AC: the
// chime 0x4B, then $a3: 0x60 at a checkpoint, a finish place call 0x33-0x38 at the end of the race). Returns 1 to skip
// them at a checkpoint in a Time Sprint: the PS1's checkpoint_passed (func_80054130) returns at once in that mode, so
// its gates are silent.
extern "C" int rush2_time_sprint_checkpoint_sound(uint8_t* rdram, recomp_context* ctx) {
    if (!rush2::time_sprint::on(rdram) || (int32_t)ctx->r7 != 0x60) return 0;
    std::printf("[time_sprint] a checkpoint passed silently\n");
    std::fflush(stdout);
    return 1;
}
