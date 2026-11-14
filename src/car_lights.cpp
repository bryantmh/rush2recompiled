// Car Lights (Graphics > Effects, include/car_lights.h, docs/graphics_effects.md#car-lights): on a night track
// (rush2::track2049::night_race: a converted course whose fog is near black, Rush 2049's night tracks and the
// PlayStation SF Rush's night versions), every race car's headlamps and tail lamps light the road and whatever else is
// round them from where the lamps are on the car, and the lamps themselves glow. The tail lamps are dim while the car
// drives and about five times brighter while it brakes (Rush 2's rule: the car's flags 0x1000 set and 0x10 clear,
// func_800599A0). A lamp goes out once its corner's panel is damaged at all (level 1, car state +0xE4), front panels
// included; a wrecked car and a ghost show none.
//
// The lamps (find, per car body: its type, parts, damage stage and paint):
//   - tail lamps: what the game itself lights when the car brakes. The body's display lists (frame, corners, top:
//     rush2::car_types::part_record) are walked with their texture state and every lit texel is put back on its
//     triangle (barycentric from the texture coordinates): Rush 2's palette lighting (CI8 entries 161-175, the ramp
//     func_800599A0 turns red), a lit copy (California Speed and Dreamcast cars, rush2::car2049::brake_light_sources:
//     the texels that differ), or a mod car's brake light lists (every triangle). The texels are grouped into lamps
//     (clusters a few inches apart), at most two a side.
//   - headlamps: nothing in the games marks them. Bright, near white texels on forward facing triangles in the front
//     quarter of the body, grouped the same way, taken as the best mirrored pair. A car with none gets a pair at its
//     front corners from its shape.
//   - by hand: assets/car_lamps.json (keyed by car, checked in: corrections from tools/car_lamps.py's contact sheet) and
//     a mod car's .json "lights" (rush2::car_lights::set_mod_lamps) replace what is found, kind by kind.
// What is found is kept on disk (track_cache::directory()/car_lamps.bin, keyed by the body's display lists, vertices,
// textures and palettes, and the build), so a car is worked out once.
//
// How each car is drawn (one rush2::battle_render slot a car, 186-193, its own model placed with the car's body pose,
// so it rides with the car through frame interpolation): a trampoline in RDRAM (0x80E1F200, 32 bytes a car) calls the
// car's display list above 16 MB (recomp::alloc, gEXSetRDRAMExtended):
//   - the lens glow: each lit lamp's triangles again over the lens, an IA8 mask of its texels over the part's own
//     texture coordinates, the lamp's color as PRIM, unshaded, unfogged, blended without writing depth;
//   - the marker (gEXSetCarLight): its vertices are the lamps, which RT64 doesn't draw but lights the view with
//     (lib/rt64 CarLightPS.hlsl, CarLightCompositePS.hlsl, FramebufferRenderer::carLightOf / submitCarLights).
// The display list is rebuilt (into the next of three buffers, so one the renderer may still read is never rewritten)
// when a lamp comes on or goes off or the brakes change.
//
// Test aids: RUSH2_CAR_LIGHTS_LOG=1 prints what is found and when lamps change; RUSH2_CAR_LIGHTS_TEST=1 lights every
// car as if it braked; RUSH2_CAR_LIGHTS_DUMP=<folder> writes each car's body and lamps as JSON for tools/car_lamps.py.

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <deque>
#include <unordered_set>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <tuple>
#include <unordered_map>
#include <vector>

#include "recomp.h"
#include "librecomp/addresses.hpp"
#include "util/file.h"

#include "battle.h"
#include "battle_render.h"
#include "car2049.h"
#include "car_lights.h"
#include "car_mods.h"
#include "car_types.h"
#include "ghost.h"
#include "rush2.h"
#include "rush2049_rom.h"
#include "track2049.h"
#include "track_cache.h"
#include "wings.h"

#include "json/json.hpp"

#define F3DEX_GBI_2
#include "rt64_extended_gbi.h"

namespace {
    constexpr int max_cars = 8;
    constexpr int first_slot = 186;               // to 193 (battle_render.h): car c at first_slot + c

    // The trampolines (0x80E1F200-0x80E1F300, src/controls_menu.cpp): car c's at + 32 c.
    constexpr uint32_t trampolines = 0x80E1F200;

    // The light (lib/rt64 CarLightPS.hlsl has the shapes and intensities): how far the headlamps and tail lamps reach
    // (feet), and the light level the converted night tracks draw their surfaces at, which the surfaces' albedo is
    // taken from (of 255).
    constexpr uint32_t head_reach = 150, tail_reach = 22;
    constexpr uint32_t surface_level = 110;
    // The lamps' colors (warm white, red) and their strength (of 255): a tail lamp lit to drive by is a fifth of its
    // braking strength.
    constexpr uint8_t head_rgb[3] = { 255, 244, 222 };
    constexpr uint8_t tail_rgb[3] = { 255, 18, 8 };
    constexpr int tail_dim = 51;
    // The lens glow (PRIM, its alpha the mask's opacity): a headlamp's, a braking tail lamp's and a dim one's.
    constexpr uint32_t lens_head = 0xFFF6E6F0u, lens_brake = 0xFF2010FFu, lens_dim = 0xB0100890u;
    // The glow round a lamp, times its lens's radius (RT64 draws it at most 4 ft): the lens itself, so the core
    // stays on the lamp (1.6 washed a livery's paint round its brake lights).
    constexpr float glow_scale = 1.0f;

    constexpr uint32_t game_state = 0x8010C0D0;   // s32: 3 and up a race
    constexpr uint32_t cars = 0x800F5470;         // physics cars
    constexpr uint32_t car_size = 0x81C;
    constexpr uint32_t car_wrecked = 0x648;       // s8
    constexpr uint32_t car_active = 0x7E4;        // s16, nonzero for a car in the race
    constexpr uint32_t car_type = 0x7EA;          // u8
    constexpr uint32_t car_corners = 0xE8;        // f32[4][3]: the collision box's corners (panel order)
    constexpr uint32_t car_states = 0x801124A0;
    constexpr uint32_t car_state_size = 0x354;
    constexpr uint32_t state_damage = 0xE4;       // u32: each panel's 2-bit damage level
    constexpr uint32_t panel_masks = 0x800CE188;  // u32 per panel (0 front right, 1 front left, 2 rear right, 3 rear left)
    constexpr uint32_t panel_shifts = 0x800CE180; // u8 per panel
    constexpr int tail_first = 161, tail_last = 175;   // Rush 2's tail light ramp (func_800599A0)
    // Bumped whenever what is found changes, so the disk cache is found again.
    constexpr uint32_t finder_version = 8;

    std::atomic_bool enabled = false;
    std::atomic_bool braking[max_cars] = {};
    const bool log_on = std::getenv("RUSH2_CAR_LIGHTS_LOG") != nullptr;
    const bool test_on = std::getenv("RUSH2_CAR_LIGHTS_TEST") != nullptr;
    const char* const dump_dir = std::getenv("RUSH2_CAR_LIGHTS_DUMP");
    // RUSH2_TEST_LAMP_CULL (test aid): 0 the lens glow two-sided, as before issue 70; b back faces culled (the wrong
    // side, see build_list). To compare the in-car views.
    const uint32_t lens_cull = [] {
        const char* v = std::getenv("RUSH2_TEST_LAMP_CULL");
        return v == nullptr ? 0x200u : v[0] == '0' ? 0u : v[0] == 'b' ? 0x400u : 0x200u;
    }();

    float read_f(uint8_t* rdram, uint32_t at) {
        uint32_t w = (uint32_t)MEM_W(0, (int32_t)at);
        float f;
        memcpy(&f, &w, 4);
        return f;
    }

    bool plain(uint32_t w) { return (w >> 24) == 0x00 || (w >> 24) == 0x80; }   // KSEG0 or physical
    uint32_t kseg0(uint32_t w) { return 0x80000000u | (w & 0xFFFFFF); }

    // ---- what a car's lamps are ---------------------------------------------------------------------------------------

    // A mask of a texture's lamp texels (IA8, the tile's texels a row, padded to 8; lit 0xFF), as the lens glow loads it.
    struct Mask {
        int width = 0, rows = 0, pitch = 0;
        std::vector<uint8_t> texels;
    };
    // A lens triangle: one of the body's triangles with lamp texels, its three vertices copied (16 bytes each, as the
    // part's list loads them) and the texture state that maps its texels.
    struct LensTriangle {
        int lamp = 0;
        int mask = 0;
        std::array<uint8_t, 48> vertices{};
        uint32_t texture_w1 = 0, wrap = 0, size0 = 0, size1 = 0;
    };
    struct Lamp {
        bool head = false;
        float pos[3] = {};   // ft, the body's frame
        float size = 0.25f;  // ft, the lens's radius
        int side = 0;        // 0 x > 0, 1 x < 0
    };
    struct Found {
        std::vector<Lamp> lamps;
        std::vector<Mask> masks;
        std::vector<LensTriangle> lens;
    };

    // ---- the disk cache -----------------------------------------------------------------------------------------------

    std::mutex cache_mutex;
    std::unordered_map<uint64_t, Found> cache;   // by content key
    bool cache_loaded = false, cache_loading = false, cache_dirty = false;
    std::chrono::steady_clock::time_point cache_saved_at{};
    constexpr uint32_t cache_magic = 0x4C524143;  // "CARL"

    std::filesystem::path cache_path() { return rush2::track_cache::directory() / "car_lamps.bin"; }

    template <class T> void put(std::vector<uint8_t>& out, const T& v) {
        const uint8_t* p = reinterpret_cast<const uint8_t*>(&v);
        out.insert(out.end(), p, p + sizeof(T));
    }
    template <class T> bool get(const std::vector<uint8_t>& in, size_t& at, T& v) {
        if (at + sizeof(T) > in.size()) return false;
        memcpy(&v, in.data() + at, sizeof(T));
        at += sizeof(T);
        return true;
    }

    std::vector<uint8_t> serialize(const std::unordered_map<uint64_t, Found>& entries) {
        std::vector<uint8_t> out;
        put(out, cache_magic);
        put(out, finder_version);
        put(out, rush2::track_cache::build());
        put(out, uint32_t(entries.size()));
        for (const auto& [key, f] : entries) {
            put(out, key);
            put(out, uint32_t(f.lamps.size()));
            for (const Lamp& l : f.lamps) put(out, l);
            put(out, uint32_t(f.masks.size()));
            for (const Mask& m : f.masks) {
                put(out, m.width), put(out, m.rows), put(out, m.pitch);
                put(out, uint32_t(m.texels.size()));
                out.insert(out.end(), m.texels.begin(), m.texels.end());
            }
            put(out, uint32_t(f.lens.size()));
            for (const LensTriangle& t : f.lens) put(out, t);
        }
        return out;
    }

    bool deserialize(const std::vector<uint8_t>& in, std::unordered_map<uint64_t, Found>& entries) {
        size_t at = 0;
        uint32_t magic = 0, version = 0, count = 0;
        uint64_t build = 0;
        if (!get(in, at, magic) || magic != cache_magic || !get(in, at, version) || version != finder_version ||
            !get(in, at, build) || build != rush2::track_cache::build() || !get(in, at, count)) {
            return false;
        }
        for (uint32_t e = 0; e < count; e++) {
            uint64_t key = 0;
            uint32_t n = 0;
            Found f;
            if (!get(in, at, key) || !get(in, at, n) || n > 64) return false;
            f.lamps.resize(n);
            for (Lamp& l : f.lamps) {
                if (!get(in, at, l)) return false;
            }
            if (!get(in, at, n) || n > 256) return false;
            f.masks.resize(n);
            for (Mask& m : f.masks) {
                uint32_t bytes = 0;
                if (!get(in, at, m.width) || !get(in, at, m.rows) || !get(in, at, m.pitch) || !get(in, at, bytes) || at + bytes > in.size()) return false;
                m.texels.assign(in.begin() + (ptrdiff_t)at, in.begin() + (ptrdiff_t)(at + bytes));
                at += bytes;
            }
            if (!get(in, at, n) || n > 4096) return false;
            f.lens.resize(n);
            for (LensTriangle& t : f.lens) {
                if (!get(in, at, t)) return false;
            }
            entries[key] = std::move(f);
        }
        return true;
    }

    // Read once on a worker; until it is in, a car is worked out on the game thread (once).
    void load_cache_async() {
        {
            std::lock_guard lock{ cache_mutex };
            if (cache_loaded || cache_loading) return;
            cache_loading = true;
        }
        std::thread([] {
            std::unordered_map<uint64_t, Found> entries;
            std::ifstream f{ cache_path(), std::ios::binary };
            if (f) {
                std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
                if (!deserialize(bytes, entries)) entries.clear();
            }
            std::lock_guard lock{ cache_mutex };
            for (auto& [k, v] : entries) cache.try_emplace(k, std::move(v));
            cache_loaded = true;
            cache_loading = false;
        }).detach();
    }

    // Written on a worker, at most every few seconds, once something new was found.
    void save_cache_async() {
        std::vector<uint8_t> bytes;
        {
            std::lock_guard lock{ cache_mutex };
            auto now = std::chrono::steady_clock::now();
            if (!cache_dirty || !cache_loaded || now - cache_saved_at < std::chrono::seconds(3)) return;
            cache_dirty = false;
            cache_saved_at = now;
            bytes = serialize(cache);
        }
        std::thread([bytes = std::move(bytes)] {
            std::error_code ec;
            std::filesystem::create_directories(cache_path().parent_path(), ec);
            std::filesystem::path tmp = cache_path();
            tmp += ".tmp";
            {
                std::ofstream f{ tmp, std::ios::binary | std::ios::trunc };
                if (!f) return;
                f.write(reinterpret_cast<const char*>(bytes.data()), (std::streamsize)bytes.size());
                if (!f) return;
            }
            std::filesystem::rename(tmp, cache_path(), ec);
        }).detach();
    }

    // ---- lamps given by hand ------------------------------------------------------------------------------------------

    std::mutex hand_mutex;
    std::map<std::string, std::vector<rush2::car_lights::LampSpec>> table;   // assets/car_lamps.json, by car key
    std::map<int, std::vector<rush2::car_lights::LampSpec>> mod_lamps;      // by type
    std::atomic_bool table_loaded = false;

    // assets/car_lamps.json: {"cars": {"<car key>": [{"kind": "head"|"tail", "at": [x, y, z], "size": r}, ...]}}.
    void load_table_async() {
        static std::atomic_bool started = false;
        if (started.exchange(true)) return;
        std::thread([] {
            std::map<std::string, std::vector<rush2::car_lights::LampSpec>> t;
            std::ifstream f{ recompui::file::get_program_path() / "assets" / "car_lamps.json" };
            if (f) {
                nlohmann::json j = nlohmann::json::parse(f, nullptr, false);
                if (j.is_object() && j.contains("cars") && j["cars"].is_object()) {
                    for (const auto& [key, list] : j["cars"].items()) {
                        if (!list.is_array()) continue;
                        std::vector<rush2::car_lights::LampSpec>& out = t[key];
                        for (const auto& e : list) {
                            if (!e.is_object() || !e.contains("at") || !e["at"].is_array() || e["at"].size() != 3) continue;
                            rush2::car_lights::LampSpec s;
                            s.head = e.value("kind", std::string()) == "head";
                            for (int k = 0; k < 3; k++) s.at[k] = e["at"][k].is_number() ? e["at"][k].get<float>() : 0.0f;
                            s.size = e.contains("size") && e["size"].is_number() ? e["size"].get<float>() : 0.25f;
                            s.lens = !e.contains("lens") || !e["lens"].is_boolean() || e["lens"].get<bool>();
                            if (e.contains("color") && e["color"].is_array() && e["color"].size() == 3) {
                                s.has_color = true;
                                for (int k = 0; k < 3; k++) s.color[k] = e["color"][k].is_number() ? e["color"][k].get<float>() : 0.0f;
                            }
                            out.push_back(s);
                        }
                    }
                }
            }
            std::lock_guard lock{ hand_mutex };
            table = std::move(t);
            table_loaded = true;
        }).detach();
    }

    // The car's name in assets/car_lamps.json: its game and its type there (a mod car by its name, its type changing
    // with the mods loaded).
    std::string car_key(int type) {
        namespace ct = rush2::car_types;
        if (type < ct::rush2_types) return "rush2:" + std::to_string(type);
        if (ct::is_2049(type)) {
            auto rom = rush2::wings::get_rom();
            return std::string(rom != nullptr && rom->is_dreamcast() ? "rush2049dc:" : "rush2049:") + std::to_string(type - ct::first_2049);
        }
        if (ct::is_calspeed(type)) return "calspeed:" + std::to_string(type - ct::first_calspeed);
        if (ct::is_mod(type)) return "mod:" + rush2::car2049::mod_car_name(type - ct::first_mod);
        return "type:" + std::to_string(type);
    }

    // A car mod livery (its own body) gets its own key: "<car key>/<livery file>".
    std::string car_key(uint8_t* rdram, int car, int type) {
        std::string key = car_key(type);
        const std::string livery = rush2::car_mods::livery_name(rdram, car);
        return livery.empty() ? key : key + "/" + livery;
    }

    // ---- finding a car's lamps ----------------------------------------------------------------------------------------

    // A CI texture's palette: entry i (a CI4 texel's index plus `offset`) at tlut + 2 i, RGBA16, for lo <= i <= hi.
    struct Palette {
        uint32_t tlut = 0;
        int offset = 0, lo = 0, hi = 255;
        bool entry(uint8_t* rdram, int i, uint32_t& w) const {
            i += offset;
            if (tlut == 0 || i < lo || i > hi) return false;
            w = ((uint32_t)MEM_BU(0, (int32_t)(tlut + uint32_t(i) * 2)) << 8) | MEM_BU(0, (int32_t)(tlut + uint32_t(i) * 2 + 1));
            return true;
        }
    };

    // A texel's color (0-255 RGB) from a texture in RDRAM, `row` bytes a row, of format fmt / size siz, a CI texture's
    // from its palette. False if it can't be read.
    bool texel_color(uint8_t* rdram, uint32_t image, int row, int fmt, int siz, int u, int v, const Palette& pal, float rgb[3],
                     float* alpha = nullptr) {
        auto byte = [&](uint32_t a) { return (uint32_t)MEM_BU(0, (int32_t)a); };
        float a_out = 255.0f;
        auto rgba16 = [&](uint32_t w) {
            rgb[0] = float((w >> 11) & 31) * 255.0f / 31.0f;
            rgb[1] = float((w >> 6) & 31) * 255.0f / 31.0f;
            rgb[2] = float((w >> 1) & 31) * 255.0f / 31.0f;
            a_out = (w & 1) ? 255.0f : 0.0f;
        };
        struct Out { float*& alpha; float& a; ~Out() { if (alpha) *alpha = a; } } out{ alpha, a_out };
        uint32_t at = image + uint32_t(v * row);
        switch (siz) {
        case 0: {
            uint32_t b = byte(at + uint32_t(u / 2));
            uint32_t n = (u & 1) ? (b & 15) : (b >> 4);
            if (fmt == 2) {
                uint32_t w;
                if (!pal.entry(rdram, int(n), w)) return false;
                rgba16(w);
            }
            else if (fmt == 3) rgb[0] = rgb[1] = rgb[2] = float((n >> 1) * 255 / 7), a_out = (n & 1) ? 255.0f : 0.0f;
            else rgb[0] = rgb[1] = rgb[2] = a_out = float(n * 17);
            return true;
        }
        case 1: {
            uint32_t b = byte(at + uint32_t(u));
            if (fmt == 2) {
                uint32_t w;
                if (!pal.entry(rdram, int(b), w)) return false;
                rgba16(w);
            }
            else if (fmt == 3) rgb[0] = rgb[1] = rgb[2] = float((b >> 4) * 17), a_out = float((b & 15) * 17);
            else rgb[0] = rgb[1] = rgb[2] = a_out = float(b);
            return true;
        }
        case 2: {
            uint32_t w = (byte(at + uint32_t(u * 2)) << 8) | byte(at + uint32_t(u * 2 + 1));
            if (fmt == 3) rgb[0] = rgb[1] = rgb[2] = float(w >> 8), a_out = float(w & 0xFF);
            else rgba16(w);
            return true;
        }
        case 3:
            for (int k = 0; k < 3; k++) rgb[k] = float(byte(at + uint32_t(u * 4 + k)));
            a_out = float(byte(at + uint32_t(u * 4 + 3)));
            return true;
        }
        return false;
    }

    struct Finder {
        uint8_t* rdram;
        const rush2::car2049::BrakeLightSources& sources;
        // Texture state.
        struct Tile { int fmt = 0, siz = 0, line = 0, tmem = 0, palette = 0; float uls = 0, ult = 0, lrt = 0; uint32_t wrap = 0, size0 = 0, size1 = 0; };
        Tile tiles[8];
        uint32_t timg = 0;
        int timg_siz = 0, timg_width = 0;
        std::map<int, uint32_t> tmem_image;   // TMEM word -> the image loaded there (TLUTs at 256 and up)
        int tex_tile = 0;
        bool texture_on = true;    // the parts draw with the texture their caller turned on
        float sscale = 1.0f, tscale = 1.0f;
        uint32_t texture_w1 = 0;
        std::array<uint32_t, 64> slots{};
        int list_index = 0;
        bool whole = false;
        std::unordered_set<uint32_t>* visited = nullptr;   // every command walked (the body's: which brake switches it has)
        std::vector<uint32_t> skip;                         // lists not to enter (the brake lists, when lit)

        // Every textured triangle of the body (and every triangle of a brake light list), as drawn.
        struct Tri {
            int list = 0;
            bool whole = false;
            uint32_t v[3] = {};
            float p[3][3] = {};   // ft
            float uv[3][2] = {};  // texels, from the tile's corner
            uint32_t image = 0;
            Palette palette;
            int row = 0, rows = 0, width = 0, fmt = 0, siz = 0;
            uint32_t texture_w1 = 0, wrap = 0, size0 = 0, size1 = 0;
        };
        std::vector<Tri> tris;

        Finder(uint8_t* r, const rush2::car2049::BrakeLightSources& s) : rdram(r), sources(s) {}

        int raw_tris = 0, untextured = 0;
        Palette car_palette;   // the car's own (its palette record), for textures whose lists load none

        void triangle(uint32_t a, uint32_t b, uint32_t c) {
            raw_tris++;
            Tri t;
            t.list = list_index;
            t.whole = whole;
            uint32_t v[3] = { slots[(a / 2) & 63], slots[(b / 2) & 63], slots[(c / 2) & 63] };
            for (int k = 0; k < 3; k++) {
                if (v[k] == 0) return;
                t.v[k] = v[k];
                for (int j = 0; j < 3; j++) t.p[k][j] = (float)(int16_t)MEM_H(0, (int32_t)(v[k] + j * 2)) / 16.0f;
                t.uv[k][0] = (float)(int16_t)MEM_H(0, (int32_t)(v[k] + 8)) * sscale / 32.0f;
                t.uv[k][1] = (float)(int16_t)MEM_H(0, (int32_t)(v[k] + 10)) * tscale / 32.0f;
            }
            // The texture, and where the triangle samples it (a brake light list's too: a mod's brake lights are
            // textured quads, often spanning both lamps, whose opaque texels are the lamps).
            auto texture = [&]() -> bool {
                const Tile& tile = tiles[tex_tile & 7];
                auto img = tmem_image.find(tile.tmem);
                if (img == tmem_image.end()) return false;
                t.image = img->second;
                t.row = tile.line * 8;
                t.rows = int(tile.lrt - tile.ult) + 1;
                t.fmt = tile.fmt;
                t.siz = tile.siz;
                t.width = tile.siz == 0 ? t.row * 2 : t.row / (1 << (tile.siz - 1));
                if (t.row <= 0 || t.rows <= 0 || t.row > 4096 || t.rows > 1024 || t.width <= 0) return false;
                if (!rush2::in_game_ram(t.image + uint32_t(t.row * t.rows))) return false;
                if (tile.fmt == 2) {
                    // The palette: one these lists load (CI8 the whole TLUT at 256; CI4 palette p's 16 entries at
                    // 256 + 16 p), or else the car's own, which the game loads before drawing the car (its record:
                    // entries first to last).
                    const int want = 256 + (tile.siz == 0 ? tile.palette * 16 : 0);
                    auto it = tmem_image.upper_bound(want);
                    if (it != tmem_image.begin() && std::prev(it)->first >= 256) {
                        t.palette.tlut = std::prev(it)->second + uint32_t(want - std::prev(it)->first) * 2;
                    }
                    else if (car_palette.tlut != 0) {
                        t.palette = car_palette;
                        t.palette.offset = tile.siz == 0 ? tile.palette * 16 : 0;
                    }
                }
                for (int k = 0; k < 3; k++) t.uv[k][0] -= tile.uls, t.uv[k][1] -= tile.ult;
                t.texture_w1 = texture_w1;
                t.wrap = tile.wrap;
                t.size0 = tile.size0;
                t.size1 = tile.size1;
                return true;
            };
            if (!whole) {
                if (!texture_on) {
                    untextured++;
                    return;
                }
                if (!texture()) return;
            }
            else if (!texture_on || !texture()) {
                t.image = 0;
                t.width = 0;
            }
            tris.push_back(t);
        }

        // F3DEX2 commands (and the 2049 conversion's call, as battle.cpp's body_triangles reads them).
        void walk(uint32_t dl, int depth) {
            if (depth > 8 || !plain(dl)) return;
            uint32_t at = kseg0(dl);
            for (int n = 0; n < 8192; n++, at += 8) {
                if (!rush2::in_game_ram(at + 8)) return;
                if (visited != nullptr) visited->insert(at);
                uint32_t w0 = (uint32_t)MEM_W(0, (int32_t)at), w1 = (uint32_t)MEM_W(4, (int32_t)at);
                switch (w0 >> 24) {
                case 0x01: {   // G_VTX
                    uint32_t count = (w0 >> 12) & 0xFF, end = (w0 >> 1) & 0x7F;
                    uint32_t a = kseg0(w1);
                    bool ok = plain(w1) && rush2::in_game_ram(a + count * 16);
                    for (uint32_t k = 0; k < count; k++) slots[(end - count + k) & 63] = ok ? a + k * 16 : 0;
                    break;
                }
                case 0x05: triangle((w0 >> 16) & 0xFF, (w0 >> 8) & 0xFF, w0 & 0xFF); break;
                case 0x06:
                case 0x07:
                    triangle((w0 >> 16) & 0xFF, (w0 >> 8) & 0xFF, w0 & 0xFF);
                    triangle((w1 >> 16) & 0xFF, (w1 >> 8) & 0xFF, w1 & 0xFF);
                    break;
                case 0xD7:     // G_TEXTURE
                    tex_tile = (w0 >> 8) & 7;
                    texture_on = ((w0 >> 1) & 0x7F) != 0;
                    texture_w1 = w1;
                    sscale = float(w1 >> 16) / 65536.0f;
                    tscale = float(w1 & 0xFFFF) / 65536.0f;
                    break;
                case 0xFD:     // G_SETTIMG
                    timg = plain(w1) ? kseg0(w1) : 0;
                    timg_siz = (w0 >> 19) & 3;
                    timg_width = int(w0 & 0xFFF) + 1;
                    break;
                case 0xF5: {   // G_SETTILE
                    Tile& t = tiles[(w1 >> 24) & 7];
                    t.fmt = (w0 >> 21) & 7;
                    t.siz = (w0 >> 19) & 3;
                    t.line = (w0 >> 9) & 0x1FF;
                    t.tmem = w0 & 0x1FF;
                    t.palette = (w1 >> 20) & 15;
                    t.wrap = w1 & 0x000FFFFF;   // cmt, maskt, shiftt, cms, masks, shifts
                    break;
                }
                case 0xF2: {   // G_SETTILESIZE
                    Tile& t = tiles[(w1 >> 24) & 7];
                    t.uls = float((w0 >> 12) & 0xFFF) / 4.0f;
                    t.ult = float(w0 & 0xFFF) / 4.0f;
                    t.lrt = float(w1 & 0xFFF) / 4.0f;
                    t.size0 = w0 & 0x00FFFFFF;
                    t.size1 = w1 & 0x00FFFFFF;
                    break;
                }
                case 0xF3:     // G_LOADBLOCK: the image from its start
                case 0xF0:     // G_LOADTLUT
                    if (timg != 0) tmem_image[tiles[(w1 >> 24) & 7].tmem] = timg;
                    break;
                case 0xF4: {   // G_LOADTILE: from its corner
                    if (timg == 0) break;
                    uint32_t uls = ((w0 >> 12) & 0xFFF) / 4, ult = (w0 & 0xFFF) / 4;
                    uint32_t bytes = timg_siz == 0 ? 0 : (1u << (timg_siz - 1));
                    tmem_image[tiles[(w1 >> 24) & 7].tmem] = timg + ult * uint32_t(timg_width) * bytes + uls * bytes;
                    break;
                }
                case 0xDE:     // G_DL
                    if (std::find(skip.begin(), skip.end(), kseg0(w1)) == skip.end()) walk(w1, depth + 1);
                    if (((w0 >> 16) & 0xFF) != 0) return;
                    break;
                case 0xE0:     // the 2049 conversion's call
                    if (((w0 >> 16) & 0xFF) == 1) walk(w1, depth + 1);
                    break;
                case 0xDF: return;
                default: break;
                }
            }
        }
    };

    // Whether texel (u, v) of a tri's texture is lit by the brakes (see the top).
    struct TailRule {
        uint8_t* rdram;
        const rush2::car2049::BrakeLightSources& sources;
        std::map<uint32_t, std::pair<uint32_t, const std::vector<uint8_t>*>> lit_of;   // image -> its lit copy

        TailRule(uint8_t* r, const rush2::car2049::BrakeLightSources& s) : rdram(r), sources(s) {
            for (const auto& [a, b] : sources.lit_copies) {
                lit_of[a] = { b, nullptr };
                lit_of[b] = { a, nullptr };
            }
            for (const auto& [a, bytes] : sources.lit_bytes) lit_of[a] = { 0, &bytes };
        }

        bool lit(const Finder::Tri& t, int u, int v) const {
            const int bytes = t.siz == 0 ? 1 : (1 << (t.siz - 1));
            const uint32_t off = uint32_t(v * t.row + (t.siz == 0 ? u / 2 : u * bytes));
            if (sources.palette && t.fmt == 2 && t.siz == 1) {
                int index = MEM_BU(0, (int32_t)(t.image + off));
                if (index >= tail_first && index <= tail_last) return true;
            }
            auto it = lit_of.find(t.image);
            if (it == lit_of.end()) return false;
            for (int b = 0; b < bytes; b++) {
                uint8_t a = MEM_BU(0, (int32_t)(t.image + off + uint32_t(b)));
                if (it->second.first != 0) {
                    if (a != MEM_BU(0, (int32_t)(it->second.first + off + uint32_t(b)))) return true;
                }
                else if (it->second.second != nullptr && off + uint32_t(b) < it->second.second->size()) {
                    if (a != (*it->second.second)[off + uint32_t(b)]) return true;
                }
            }
            return false;
        }
    };

    // A texel put back on its triangle: where it is, which tri and which texel.
    // reach: how far (ft) a neighbor may be and still be the same lamp: a texel's own size on the body, a brake light
    // list triangle's corner only its own triangle's corners and corners at the same place.
    struct Point {
        float p[3];
        int tri;
        int u, v;
        float weight;
        float reach;
        bool whole = false;
    };

    // How big (ft) a texel of the triangle's texture is on the body.
    float texel_size(const Finder::Tri& t) {
        float e1[3], e2[3];
        for (int j = 0; j < 3; j++) e1[j] = t.p[1][j] - t.p[0][j], e2[j] = t.p[2][j] - t.p[0][j];
        const float n[3] = { e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0] };
        const float area = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
        const float uv_area = std::fabs((t.uv[1][0] - t.uv[0][0]) * (t.uv[2][1] - t.uv[0][1]) - (t.uv[2][0] - t.uv[0][0]) * (t.uv[1][1] - t.uv[0][1]));
        return uv_area > 1e-6f ? std::sqrt(area / uv_area) : 0.1f;
    }

    // Which texel of a `size` long axis a texture coordinate reads, as the RDP addresses it (the tile's cm and mask:
    // G_SETTILE's cms/masks or cmt/maskt): clamped to the tile with the clamp bit, then wrapped to 2^mask texels, every
    // other repeat flipped with the mirror bit. California Speed's bodies mirror one texture half onto both sides of
    // the car; wrapping those plainly put the far side's lamps next to the middle instead of at its own corner.
    int texel_index(int s, int cm, int mask, int size) {
        if (cm & 2) s = std::clamp(s, 0, size - 1);
        if (mask > 0) {
            const int period = 1 << mask;
            const bool flip = (cm & 1) && ((s >> mask) & 1);
            s &= period - 1;
            if (flip) s = period - 1 - s;
        }
        s %= size;
        return s < 0 ? s + size : s;
    }
    int texel_u(const Finder::Tri& t, float u) { return texel_index(int(std::floor(u)), (t.wrap >> 8) & 3, (t.wrap >> 4) & 15, t.width); }
    int texel_v(const Finder::Tri& t, float v) { return texel_index(int(std::floor(v)), (t.wrap >> 18) & 3, (t.wrap >> 14) & 15, t.rows); }

    // Each texel inside the triangle's texture coordinates (and its repeats, for wrapped textures), with where it lands.
    template <class F> void for_texels(const Finder::Tri& t, F&& f) {
        const float (&uv)[3][2] = t.uv;
        const float d = (uv[1][1] - uv[2][1]) * (uv[0][0] - uv[2][0]) + (uv[2][0] - uv[1][0]) * (uv[0][1] - uv[2][1]);
        float lo[2] = { std::min({ uv[0][0], uv[1][0], uv[2][0] }), std::min({ uv[0][1], uv[1][1], uv[2][1] }) };
        float hi[2] = { std::max({ uv[0][0], uv[1][0], uv[2][0] }), std::max({ uv[0][1], uv[1][1], uv[2][1] }) };
        if (std::fabs(d) < 1e-6f || hi[0] - lo[0] > 1024 || hi[1] - lo[1] > 1024) return;
        for (float ty = std::floor(lo[1]) + 0.5f; ty <= hi[1]; ty += 1.0f) {
            for (float tx = std::floor(lo[0]) + 0.5f; tx <= hi[0]; tx += 1.0f) {
                float l0 = ((uv[1][1] - uv[2][1]) * (tx - uv[2][0]) + (uv[2][0] - uv[1][0]) * (ty - uv[2][1])) / d;
                float l1 = ((uv[2][1] - uv[0][1]) * (tx - uv[2][0]) + (uv[0][0] - uv[2][0]) * (ty - uv[2][1])) / d;
                float l2 = 1.0f - l0 - l1;
                if (l0 < -1e-3f || l1 < -1e-3f || l2 < -1e-3f) continue;
                const int u = texel_u(t, tx), v = texel_v(t, ty);
                float q[3];
                for (int j = 0; j < 3; j++) q[j] = l0 * t.p[0][j] + l1 * t.p[1][j] + l2 * t.p[2][j];
                f(u, v, q);
            }
        }
    }

    struct Cluster {
        std::vector<int> points;
        float c[3] = {};
        float radius = 0.0f;
        float weight = 0.0f;
    };

    // Points as groups: two points are the same lamp when nearer than 1.5 times the larger of their reaches (texels
    // next to each other), or when they are corners of one brake light list triangle. Lamps a texel or two apart
    // (round lamps side by side) stay apart.
    std::vector<Cluster> clusters_of(const std::vector<Point>& pts) {
        const size_t n = pts.size();
        std::vector<int> parent(n);
        for (size_t i = 0; i < n; i++) parent[i] = int(i);
        auto root = [&](int i) {
            while (parent[i] != i) i = parent[i] = parent[parent[i]];
            return i;
        };
        std::map<int, int> whole_tri;
        for (size_t i = 0; i < n; i++) {
            if (!pts[i].whole) continue;
            auto [it, fresh] = whole_tri.try_emplace(pts[i].tri, int(i));
            if (!fresh) parent[root(int(i))] = root(it->second);
        }
        float reach = 0.02f;
        for (const Point& p : pts) reach = std::max(reach, std::min(1.5f * p.reach + 0.01f, 0.25f));
        std::map<std::tuple<int, int, int>, std::vector<int>> grid;
        auto cell = [&](const float p[3]) {
            return std::make_tuple(int(std::floor(p[0] / reach)), int(std::floor(p[1] / reach)), int(std::floor(p[2] / reach)));
        };
        for (size_t i = 0; i < n; i++) grid[cell(pts[i].p)].push_back(int(i));
        for (size_t i = 0; i < n; i++) {
            auto [cx, cy, cz] = cell(pts[i].p);
            for (int dx = -1; dx <= 1; dx++) for (int dy = -1; dy <= 1; dy++) for (int dz = -1; dz <= 1; dz++) {
                auto it = grid.find({ cx + dx, cy + dy, cz + dz });
                if (it == grid.end()) continue;
                for (int j : it->second) {
                    if (j <= int(i)) continue;
                    float d2 = 0.0f;
                    for (int k = 0; k < 3; k++) d2 += (pts[i].p[k] - pts[j].p[k]) * (pts[i].p[k] - pts[j].p[k]);
                    const float r = std::min(1.5f * std::max(pts[i].reach, pts[j].reach) + 0.01f, 0.25f);
                    if (d2 <= r * r) parent[root(j)] = root(int(i));
                }
            }
        }
        std::map<int, Cluster> by_root;
        for (size_t i = 0; i < n; i++) {
            Cluster& c = by_root[root(int(i))];
            c.points.push_back(int(i));
            for (int k = 0; k < 3; k++) c.c[k] += pts[i].p[k] * pts[i].weight;
            c.weight += pts[i].weight;
        }
        std::vector<Cluster> out;
        for (auto& [r, c] : by_root) {
            for (int k = 0; k < 3; k++) c.c[k] /= std::max(c.weight, 1e-6f);
            // The radius of a disc with the points' spread (a uniform disc's RMS distance is its radius over sqrt 2), so
            // a stray texel doesn't swell the lamp the way the farthest point would.
            float sum = 0.0f;
            for (int i : c.points) {
                for (int k = 0; k < 3; k++) sum += (pts[i].p[k] - c.c[k]) * (pts[i].p[k] - c.c[k]);
            }
            c.radius = std::sqrt(2.0f * sum / float(c.points.size()));
            out.push_back(std::move(c));
        }
        std::sort(out.begin(), out.end(), [](const Cluster& a, const Cluster& b) { return a.weight > b.weight; });
        return out;
    }

    // The body's extent (ft).
    struct Bounds {
        float lo[3] = { 1e9f, 1e9f, 1e9f }, hi[3] = { -1e9f, -1e9f, -1e9f };
        float mid(int k) const { return (lo[k] + hi[k]) * 0.5f; }
        float span(int k) const { return hi[k] - lo[k]; }
    };

    // Car i's body lists (frame, the four corners, the top).
    void body_lists(uint8_t* rdram, int i, uint32_t (&lists)[6]) {
        static constexpr int parts[] = { rush2::car_types::part_frame, rush2::car_types::part_front_right, rush2::car_types::part_front_left,
                                         rush2::car_types::part_rear_right, rush2::car_types::part_rear_left, rush2::car_types::part_top };
        for (size_t k = 0; k < std::size(parts); k++) {
            uint32_t record = rush2::car_types::part_record(rdram, i, parts[k]);
            lists[k] = record != 0 && rush2::in_game_ram(record) ? (uint32_t)MEM_W(0xC, (int32_t)record) : 0;
        }
    }

    // What the body is drawn from, cheaply: the lists' commands, the vertices and the first bytes of the textures and
    // palettes they load. Equal keys find equal lamps.
    struct KeyWalker {
        uint8_t* rdram;
        uint64_t h;
        void bytes(uint32_t at, uint32_t n) {
            for (uint32_t k = 0; k + 4 <= n; k += 4) {
                if (!rush2::in_game_ram(at + k + 4)) return;
                h = (h ^ (uint32_t)MEM_W(0, (int32_t)(at + k))) * 0x100000001B3ull;
            }
        }
        void walk(uint32_t dl, int depth) {
            if (depth > 8 || !plain(dl)) return;
            uint32_t at = kseg0(dl);
            uint32_t timg = 0;
            for (int n = 0; n < 8192; n++, at += 8) {
                if (!rush2::in_game_ram(at + 8)) return;
                uint32_t w0 = (uint32_t)MEM_W(0, (int32_t)at), w1 = (uint32_t)MEM_W(4, (int32_t)at);
                h = ((h ^ w0) * 0x100000001B3ull ^ w1) * 0x100000001B3ull;
                switch (w0 >> 24) {
                case 0x01: if (plain(w1)) bytes(kseg0(w1), ((w0 >> 12) & 0xFF) * 16); break;
                case 0xFD: timg = plain(w1) ? kseg0(w1) : 0; break;
                case 0xF3: case 0xF4: if (timg) bytes(timg, 512); break;
                case 0xF0: if (timg) bytes(timg, 320); break;   // not entries 161-175: braking relights them
                case 0xDE: walk(w1, depth + 1); if (((w0 >> 16) & 0xFF) != 0) return; break;
                case 0xE0: if (((w0 >> 16) & 0xFF) == 1) walk(w1, depth + 1); break;
                case 0xDF: return;
                default: break;
                }
            }
        }
    };

    uint64_t content_key(uint8_t* rdram, int type, const uint32_t (&lists)[6], const rush2::car2049::BrakeLightSources& s) {
        KeyWalker k{ rdram, 0xCBF29CE484222325ull ^ (uint64_t(finder_version) << 32) ^ uint64_t(type + 1) };
        k.h = (k.h ^ uint64_t(s.palette)) * 0x100000001B3ull;
        for (uint32_t l : lists) k.walk(l, 0);
        for (uint32_t l : s.lists) k.walk(l, 0);
        for (const auto& [a, b] : s.lit_copies) k.h = ((k.h ^ a) * 0x100000001B3ull ^ b) * 0x100000001B3ull;
        for (const auto& [a, bytes] : s.lit_bytes) {
            k.h = (k.h ^ a) * 0x100000001B3ull;
            k.h = rush2::track_cache::hash(bytes.data(), bytes.size(), k.h);
        }
        return k.h;
    }

    // Body triangles for the dump: positions and a color per quarter (the texel at each sub-triangle's middle).
    void dump(uint8_t* rdram, const std::string& key, int type, const Finder& f, const Found& found, const std::vector<std::string>& origin) {
        if (dump_dir == nullptr) return;
        std::string file = key;
        for (char& c : file) if (!isalnum((unsigned char)c) && c != '-' && c != '_') c = '_';
        std::filesystem::path path = std::filesystem::path(dump_dir) / (file + ".json");
        std::error_code ec;
        if (std::filesystem::exists(path, ec)) return;
        std::filesystem::create_directories(dump_dir, ec);
        nlohmann::json j;
        j["key"] = key;
        j["type"] = type;
        nlohmann::json tris = nlohmann::json::array();
        for (const Finder::Tri& t : f.tris) {
            if (t.whole && t.image != 0) continue;   // a textured brake light quad would hide the body; its lamps are marked
            nlohmann::json e;
            e["p"] = { t.p[0][0], t.p[0][1], t.p[0][2], t.p[1][0], t.p[1][1], t.p[1][2], t.p[2][0], t.p[2][1], t.p[2][2] };
            nlohmann::json colors = nlohmann::json::array();
            // An n x n grid of sub-triangles (tools/car_lamps.py splits them the same way): for each row i and step j,
            // the one pointing up, then (but for the last of the row) the one pointing down; each sampled at its middle.
            constexpr int n = 16;
            std::vector<std::array<float, 3>> sub;
            for (int a = 0; a < n; a++) {
                for (int c = 0; c < n - a; c++) {
                    sub.push_back({ (a + 1.0f / 3.0f), (c + 1.0f / 3.0f), 0.0f });
                    if (a + c < n - 1) sub.push_back({ (a + 2.0f / 3.0f), (c + 2.0f / 3.0f), 0.0f });
                }
            }
            e["n"] = n;
            for (auto& g : sub) {
                // Grid coordinates (along B - A, along C - A) to weights of A, B, C.
                const float wb = g[0] / n, wc = g[1] / n;
                const float b[3] = { 1.0f - wb - wc, wb, wc };
                float rgb[3] = { 128, 128, 128 };
                if (t.image != 0) {
                    float u = b[0] * t.uv[0][0] + b[1] * t.uv[1][0] + b[2] * t.uv[2][0];
                    float v = b[0] * t.uv[0][1] + b[1] * t.uv[1][1] + b[2] * t.uv[2][1];
                    const int iu = texel_u(t, u), iv = texel_v(t, v);
                    if (!texel_color(rdram, t.image, t.row, t.fmt, t.siz, iu, iv, t.palette, rgb)) rgb[0] = rgb[1] = rgb[2] = 128;
                }
                else rgb[0] = 255, rgb[1] = 40, rgb[2] = 40;
                colors.push_back({ int(rgb[0]), int(rgb[1]), int(rgb[2]) });
            }
            e["c"] = colors;
            tris.push_back(e);
        }
        j["tris"] = tris;
        // The brake light lists' triangles: their texture and how many of their texels count as lit (a review aid).
        nlohmann::json brake = nlohmann::json::array();
        for (const Finder::Tri& t : f.tris) {
            if (!t.whole) continue;
            int texels = 0, lit = 0;
            if (t.image != 0) {
                for_texels(t, [&](int u, int v, const float*) {
                    float rgb[3];
                    texels++;
                    if (texel_color(rdram, t.image, t.row, t.fmt, t.siz, u, v, t.palette, rgb) && std::max({ rgb[0], rgb[1], rgb[2] }) >= 48.0f) lit++;
                });
            }
            brake.push_back({ { "p", { t.p[0][0], t.p[0][1], t.p[0][2], t.p[1][0], t.p[1][1], t.p[1][2], t.p[2][0], t.p[2][1], t.p[2][2] } },
                              { "uv", { t.uv[0][0], t.uv[0][1], t.uv[1][0], t.uv[1][1], t.uv[2][0], t.uv[2][1] } },
                              { "image", t.image }, { "fmt", t.fmt }, { "siz", t.siz }, { "width", t.width }, { "rows", t.rows },
                              { "wrap", t.wrap }, { "texels", texels }, { "lit", lit } });
        }
        j["brake"] = brake;
        nlohmann::json lamps = nlohmann::json::array();
        for (size_t k = 0; k < found.lamps.size(); k++) {
            const Lamp& l = found.lamps[k];
            lamps.push_back({ { "kind", l.head ? "head" : "tail" }, { "at", { l.pos[0], l.pos[1], l.pos[2] } }, { "size", l.size },
                              { "from", k < origin.size() ? origin[k] : "" } });
        }
        j["lamps"] = lamps;
        std::ofstream out{ path };
        out << j.dump(1);
    }

    // Works out a car's lamps from its body (see the top).
    Found work_out(uint8_t* rdram, int i, int type, const uint32_t (&lists)[6], const rush2::car2049::BrakeLightSources& sources,
                   const std::string& key, const std::vector<rush2::car_lights::LampSpec>* hand) {
        Finder f(rdram, sources);
        {
            const uint32_t record = rush2::car_types::car_palette_records + uint32_t(i) * 0x18;
            const uint32_t data = (uint32_t)MEM_W(0x14, (int32_t)record);
            const int first = MEM_BU(0, (int32_t)(record + 0x10)), last = MEM_BU(0, (int32_t)(record + 0x11));
            if (plain(data) && rush2::in_game_ram(kseg0(data) + 512) && last >= first) {
                f.car_palette.tlut = kseg0(data) - uint32_t(first) * 2;
                f.car_palette.lo = first;
                f.car_palette.hi = last;
            }
        }
        // The body, then the brake lights it switches: a car's copy holds every paint scheme's brake lists (and a
        // livery's its own), and only those whose switch is in the lists the body draws are its lights (the others put
        // lamps between and beside the real ones). A lit switch's call isn't entered as body.
        std::unordered_set<uint32_t> visited;
        f.visited = &visited;
        for (uint32_t list : sources.lists) f.skip.push_back(kseg0(list));
        for (int k = 0; k < 6; k++) {
            f.list_index = k;
            // A 2049 car's corners are one list.
            if (lists[k] != 0 && std::find(lists, lists + k, lists[k]) == lists + k) f.walk(lists[k], 0);
        }
        f.visited = nullptr;
        f.whole = true;
        f.texture_on = true;
        int brake_lists = 0;
        for (size_t k = 0; k < sources.lists.size(); k++) {
            if (k < sources.switches.size() && !visited.count(kseg0(sources.switches[k]))) continue;
            f.walk(sources.lists[k], 0);
            brake_lists++;
        }
        f.whole = false;
        Bounds b;
        for (const Finder::Tri& t : f.tris) {
            if (t.whole) continue;
            for (int v = 0; v < 3; v++) for (int k = 0; k < 3; k++) b.lo[k] = std::min(b.lo[k], t.p[v][k]), b.hi[k] = std::max(b.hi[k], t.p[v][k]);
        }
        Found found;
        std::vector<std::string> origin;
        if (log_on) {
            std::printf("[car_lights] car %d lists %08X %08X %08X %08X %08X %08X: %d triangles drawn, %d untextured\n", i, lists[0],
                        lists[1], lists[2], lists[3], lists[4], lists[5], f.raw_tris, f.untextured);
        }
        if (b.lo[0] > b.hi[0]) return found;

        const float center[3] = { b.mid(0), b.mid(1), b.mid(2) };
        // A triangle's unit normal turned away from the body's middle (false for a degenerate one).
        auto outward = [&](const Finder::Tri& t, float n[3]) {
            float mid[3], e1[3], e2[3];
            for (int j = 0; j < 3; j++) mid[j] = (t.p[0][j] + t.p[1][j] + t.p[2][j]) / 3.0f, e1[j] = t.p[1][j] - t.p[0][j], e2[j] = t.p[2][j] - t.p[0][j];
            n[0] = e1[1] * e2[2] - e1[2] * e2[1], n[1] = e1[2] * e2[0] - e1[0] * e2[2], n[2] = e1[0] * e2[1] - e1[1] * e2[0];
            const float len = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
            if (len <= 1e-9f) return false;
            for (int j = 0; j < 3; j++) n[j] /= len;
            if ((mid[0] - center[0]) * n[0] + (mid[1] - center[1]) * n[1] + (mid[2] - center[2]) * n[2] < 0.0f) for (int j = 0; j < 3; j++) n[j] = -n[j];
            return true;
        };

        // Tail lamps: the lit texels, or a brake light list's triangles by area, on triangles facing backward (lit
        // texels on a side face are side markers or the texture's spill, not the lamp).
        constexpr float tail_facing = -0.25f;
        TailRule rule(rdram, sources);
        std::vector<Point> tail_points;
        // A textured brake light's lamps are its bright texels: the textures are soft glows, a red core in near black
        // (the transparency comes from their full-size replacements; the N64 copy's alpha bit isn't kept reliably).
        float brake_top = 0.0f;
        for (const Finder::Tri& t : f.tris) {
            if (!t.whole || t.image == 0) continue;
            for_texels(t, [&](int u, int v, const float*) {
                float rgb[3];
                if (texel_color(rdram, t.image, t.row, t.fmt, t.siz, u, v, t.palette, rgb)) brake_top = std::max({ brake_top, rgb[0], rgb[1], rgb[2] });
            });
        }
        const float brake_lit = std::max(48.0f, 0.5f * brake_top);
        for (int ti = 0; ti < (int)f.tris.size(); ti++) {
            const Finder::Tri& t = f.tris[ti];
            float facing[3];
            if (!outward(t, facing) || facing[2] > tail_facing) continue;
            if (t.whole && t.image != 0) {
                // A textured brake light: its bright texels (a quad's dark rest isn't lamp).
                const float size = texel_size(t);
                bool any = false;
                for_texels(t, [&](int u, int v, const float q[3]) {
                    float rgb[3];
                    if (!texel_color(rdram, t.image, t.row, t.fmt, t.siz, u, v, t.palette, rgb)) return;
                    if (std::max({ rgb[0], rgb[1], rgb[2] }) < brake_lit) return;
                    tail_points.push_back({ { q[0], q[1], q[2] }, ti, u, v, 1.0f, size });
                    any = true;
                });
                if (any) continue;
            }
            if (t.whole) {
                float e1[3], e2[3], mid[3];
                for (int j = 0; j < 3; j++) e1[j] = t.p[1][j] - t.p[0][j], e2[j] = t.p[2][j] - t.p[0][j], mid[j] = (t.p[0][j] + t.p[1][j] + t.p[2][j]) / 3.0f;
                float n[3] = { e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0] };
                float area = 0.5f * std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
                if (area > 0.0f) {
                    tail_points.push_back({ { mid[0], mid[1], mid[2] }, ti, -1, -1, area * 100.0f, 0.0f, true });
                    for (int v = 0; v < 3; v++) tail_points.push_back({ { t.p[v][0], t.p[v][1], t.p[v][2] }, ti, -1, -1, 0.0f, 0.01f, true });
                }
                continue;
            }
            // Only the back half holds tail lamps.
            if (std::max({ t.p[0][2], t.p[1][2], t.p[2][2] }) > b.mid(2)) continue;
            const float size = texel_size(t);
            for_texels(t, [&](int u, int v, const float q[3]) {
                if (rule.lit(t, u, v)) tail_points.push_back({ { q[0], q[1], q[2] }, ti, u, v, 1.0f, size });
            });
        }

        // Headlamp candidates: bright, near white texels on forward facing triangles in the body's front quarter.
        std::vector<Point> head_points;
        const float front_from = b.hi[2] - 0.25f * b.span(2);
        for (int ti = 0; ti < (int)f.tris.size(); ti++) {
            const Finder::Tri& t = f.tris[ti];
            if (t.whole) continue;
            if ((t.p[0][2] + t.p[1][2] + t.p[2][2]) / 3.0f < front_from) continue;
            float n[3];
            if (!outward(t, n) || n[2] < 0.35f) continue;
            const float size = texel_size(t);
            for_texels(t, [&](int u, int v, const float q[3]) {
                if (sources.palette && t.fmt == 2 && t.siz == 1) {
                    int index = MEM_BU(0, (int32_t)(t.image + uint32_t(v * t.row + u)));
                    if (index >= tail_first && index <= tail_last) return;
                }
                float rgb[3];
                if (!texel_color(rdram, t.image, t.row, t.fmt, t.siz, u, v, t.palette, rgb)) return;
                const float hi = std::max({ rgb[0], rgb[1], rgb[2] }), lo = std::min({ rgb[0], rgb[1], rgb[2] });
                const float luma = 0.299f * rgb[0] + 0.587f * rgb[1] + 0.114f * rgb[2];
                if (luma >= 175.0f && (hi - lo) <= 0.3f * hi && rgb[2] >= 0.6f * rgb[0]) head_points.push_back({ { q[0], q[1], q[2] }, ti, u, v, 1.0f, size });
            });
        }

        // The lamps: tail clusters (at most two a side), the best mirrored pair of headlamp clusters.
        std::vector<Cluster> tails = clusters_of(tail_points);
        std::vector<Cluster> heads = clusters_of(head_points);
        std::vector<std::pair<int, const Cluster*>> chosen;   // (lamp, cluster) of found lamps with lens texels
        int per_side[2] = {};
        float tail_total = 0.0f;
        for (const Cluster& c : tails) tail_total += c.weight;
        for (const Cluster& c : tails) {
            const int side = c.c[0] >= 0.0f ? 0 : 1;
            if (per_side[side] >= 2 || c.weight < 0.06f * tail_total) continue;
            // A group centered inside a lamp already taken is that lamp again (overlapping brake light quads, a lens
            // drawn twice), not a second one.
            bool inside = false;
            for (const Lamp& o : found.lamps) {
                float d2 = 0.0f;
                for (int k = 0; k < 3; k++) d2 += (o.pos[k] - c.c[k]) * (o.pos[k] - c.c[k]);
                inside |= d2 < 0.25f * o.size * o.size;
            }
            if (inside) continue;
            per_side[side]++;
            Lamp l;
            l.head = false;
            for (int k = 0; k < 3; k++) l.pos[k] = c.c[k];
            l.size = std::clamp(c.radius, 0.06f, 0.6f);
            l.side = side;
            chosen.push_back({ int(found.lamps.size()), &c });
            found.lamps.push_back(l);
            origin.push_back("lit texels");
        }
        // Mirrored pairs (across the body's middle within 0.5 ft, at the same height and depth), best first: the best
        // one, and a second one at least 40% as big that shares no group with it (quad headlamps).
        struct Pair { const Cluster* side[2]; float score; };
        std::vector<Pair> pairs;
        for (const Cluster& a : heads) {
            if (a.c[0] <= center[0] || a.radius > 0.9f || a.points.size() < 3) continue;
            for (const Cluster& c : heads) {
                if (c.c[0] >= center[0] || c.radius > 0.9f || c.points.size() < 3) continue;
                if (std::fabs(a.c[0] + c.c[0] - 2.0f * center[0]) > 0.5f || std::fabs(a.c[1] - c.c[1]) > 0.35f || std::fabs(a.c[2] - c.c[2]) > 0.5f) continue;
                if (std::fabs(a.c[0] - c.c[0]) < 0.6f) continue;
                // Not near the middle: there a pair is a stripe over the grille or the grille's own chrome.
                if (std::min(a.c[0] - center[0], center[0] - c.c[0]) < 0.35f * 0.5f * b.span(0)) continue;
                pairs.push_back({ { &a, &c }, std::min(a.weight, c.weight) });
            }
        }
        std::sort(pairs.begin(), pairs.end(), [](const Pair& x, const Pair& y) { return x.score > y.score; });
        std::vector<const Pair*> taken;
        for (const Pair& pr : pairs) {
            if (!taken.empty() && (taken.size() >= 2 || pr.score < 0.4f * taken[0]->score)) break;
            bool shared = false;
            for (const Pair* q : taken) shared |= q->side[0] == pr.side[0] || q->side[1] == pr.side[1];
            // Quad headlamps sit side by side; a second pair well below the first is chrome (hubs, bumper ends).
            if (!taken.empty() && std::fabs(pr.side[0]->c[1] - taken[0]->side[0]->c[1]) > 0.4f) continue;
            // And beside it: one nearer than 0.25 ft across is the same lamp in two groups.
            if (!taken.empty() && std::fabs(pr.side[0]->c[0] - taken[0]->side[0]->c[0]) < 0.25f) continue;
            if (!shared) taken.push_back(&pr);
        }
        for (const Pair* pr : taken) {
            for (int s = 0; s < 2; s++) {
                Lamp l;
                l.head = true;
                for (int k = 0; k < 3; k++) l.pos[k] = pr->side[s]->c[k];
                l.size = std::clamp(pr->side[s]->radius, 0.08f, 0.6f);
                l.side = s;
                chosen.push_back({ int(found.lamps.size()), pr->side[s] });
                found.lamps.push_back(l);
                origin.push_back("bright texels");
            }
        }

        // A hand placed lamp's lens: the texels within its radius on triangles facing its way, those near the entry's
        // lens color (tools/car_lamps.py pick measures it), or without one the brightest (at least 60% of the brightest
        // channel there), so a gray lens on a dark body glows and the paint round it doesn't. (The brightest alone took
        // a livery's yellow paint round its red lamps.)
        std::deque<Cluster> hand_clusters;
        auto hand_lens = [&](const Lamp& l, const rush2::car_lights::LampSpec& s) -> const Cluster* {
            std::vector<Point>& pts = l.head ? head_points : tail_points;
            const size_t first = pts.size();
            std::vector<float> bright;
            for (int ti = 0; ti < (int)f.tris.size(); ti++) {
                const Finder::Tri& t = f.tris[ti];
                if (t.whole) continue;
                float n[3];
                if (!outward(t, n) || (l.head ? n[2] < 0.35f : n[2] > tail_facing)) continue;
                bool near = false;
                for (int v = 0; v < 3 && !near; v++) {
                    float d2 = 0.0f;
                    for (int k = 0; k < 3; k++) d2 += (t.p[v][k] - l.pos[k]) * (t.p[v][k] - l.pos[k]);
                    near = d2 <= (l.size + 3.0f) * (l.size + 3.0f);
                }
                if (!near) continue;
                const float size = texel_size(t);
                for_texels(t, [&](int u, int v, const float q[3]) {
                    float d2 = 0.0f;
                    for (int k = 0; k < 3; k++) d2 += (q[k] - l.pos[k]) * (q[k] - l.pos[k]);
                    if (d2 > l.size * l.size) return;
                    float rgb[3];
                    if (!texel_color(rdram, t.image, t.row, t.fmt, t.siz, u, v, t.palette, rgb)) return;
                    pts.push_back({ { q[0], q[1], q[2] }, ti, u, v, 1.0f, size });
                    if (s.has_color) {
                        float d2c = 0.0f;
                        for (int k = 0; k < 3; k++) d2c += (rgb[k] - s.color[k]) * (rgb[k] - s.color[k]);
                        bright.push_back(d2c <= 90.0f * 90.0f ? 1.0f : 0.0f);
                    }
                    else bright.push_back(std::max({ rgb[0], rgb[1], rgb[2] }));
                });
            }
            if (bright.empty()) return nullptr;
            const float top = *std::max_element(bright.begin(), bright.end());
            if (top <= 0.0f) return nullptr;
            Cluster& c = hand_clusters.emplace_back();
            for (size_t k = 0; k < bright.size(); k++) {
                if (bright[k] >= 0.6f * top) c.points.push_back(int(first + k));
            }
            return &c;
        };

        // By hand, kind by kind; then what is still missing from the body's shape.
        if (hand != nullptr) {
            for (int kind = 0; kind < 2; kind++) {
                const bool head = kind == 0;
                bool any = false;
                for (const auto& s : *hand) any |= s.head == head;
                if (!any) continue;
                std::vector<Lamp> kept;
                std::vector<std::string> kept_origin;
                std::vector<int> remap(found.lamps.size(), -1);
                for (size_t k = 0; k < found.lamps.size(); k++) {
                    if (found.lamps[k].head == head) continue;
                    remap[k] = int(kept.size());
                    kept.push_back(found.lamps[k]);
                    kept_origin.push_back(origin[k]);
                }
                std::vector<std::pair<int, const Cluster*>> kept_chosen;
                for (auto& [lamp, c] : chosen) {
                    if (remap[lamp] >= 0) kept_chosen.push_back({ remap[lamp], c });
                }
                for (const auto& s : *hand) {
                    if (s.head != head) continue;
                    Lamp l;
                    l.head = head;
                    for (int k = 0; k < 3; k++) l.pos[k] = s.at[k];
                    l.size = std::clamp(s.size, 0.05f, 2.0f);
                    l.side = s.at[0] >= 0.0f ? 0 : 1;
                    if (s.lens) {
                        const Cluster* c = hand_lens(l, s);
                        if (c != nullptr) kept_chosen.push_back({ int(kept.size()), c });
                    }
                    kept.push_back(l);
                    kept_origin.push_back("by hand");
                }
                found.lamps = std::move(kept);
                origin = std::move(kept_origin);
                chosen = std::move(kept_chosen);
            }
        }
        for (int kind = 0; kind < 2; kind++) {
            const bool head = kind == 0;
            bool any = false;
            for (const Lamp& l : found.lamps) any |= l.head == head;
            if (any) continue;
            for (int s = 0; s < 2; s++) {
                Lamp l;
                l.head = head;
                l.pos[0] = center[0] + (s == 0 ? 1.0f : -1.0f) * 0.34f * b.span(0);
                l.pos[1] = b.lo[1] + (head ? 0.32f : 0.42f) * b.span(1);
                l.pos[2] = head ? b.hi[2] - 0.1f : b.lo[2] + 0.1f;
                // On the body: the frontmost (rearmost) surface straight ahead of (behind) that point, along z.
                bool hit = false;
                float best = 0.0f;
                for (const Finder::Tri& t : f.tris) {
                    if (t.whole) continue;
                    const float (&p)[3][3] = t.p;
                    const float d = (p[1][1] - p[2][1]) * (p[0][0] - p[2][0]) + (p[2][0] - p[1][0]) * (p[0][1] - p[2][1]);
                    if (std::fabs(d) < 1e-6f) continue;
                    const float l0 = ((p[1][1] - p[2][1]) * (l.pos[0] - p[2][0]) + (p[2][0] - p[1][0]) * (l.pos[1] - p[2][1])) / d;
                    const float l1 = ((p[2][1] - p[0][1]) * (l.pos[0] - p[2][0]) + (p[0][0] - p[2][0]) * (l.pos[1] - p[2][1])) / d;
                    const float l2 = 1.0f - l0 - l1;
                    if (l0 < 0.0f || l1 < 0.0f || l2 < 0.0f) continue;
                    const float z = l0 * p[0][2] + l1 * p[1][2] + l2 * p[2][2];
                    if (!hit || (head ? z > best : z < best)) best = z, hit = true;
                }
                if (hit) l.pos[2] = best + (head ? 0.03f : -0.03f);
                l.size = 0.22f;
                l.side = s;
                found.lamps.push_back(l);
                origin.push_back("body shape");
            }
        }

        // The lens glow: each chosen cluster's triangles, over masks of the texels of chosen clusters per texture.
        std::map<std::tuple<uint32_t, int, int, int>, int> mask_of;
        for (auto& [lamp, c] : chosen) {
            const std::vector<Point>& pts = found.lamps[lamp].head ? head_points : tail_points;
            std::map<int, bool> tri_seen;
            for (int pi : c->points) {
                const Point& p = pts[pi];
                const Finder::Tri& t = f.tris[p.tri];
                if (t.whole || p.u < 0) continue;
                auto mk = std::make_tuple(t.image, t.row, t.rows, (t.siz << 4) | t.fmt);
                auto it = mask_of.find(mk);
                if (it == mask_of.end()) {
                    Mask m;
                    m.width = t.width;
                    m.rows = t.rows;
                    m.pitch = (t.width + 7) & ~7;
                    if (m.pitch * m.rows > 4096) continue;
                    m.texels.assign(size_t(m.pitch * m.rows), 0);
                    it = mask_of.emplace(mk, int(found.masks.size())).first;
                    found.masks.push_back(std::move(m));
                }
                Mask& m = found.masks[it->second];
                if (p.u < m.width && p.v < m.rows) m.texels[size_t(p.v * m.pitch + p.u)] = 0xFF;
                if (tri_seen[p.tri]) continue;
                tri_seen[p.tri] = true;
                LensTriangle lt;
                lt.lamp = lamp;
                lt.mask = it->second;
                // Wound counterclockwise seen from outside the body (the normal (B - A) x (C - A) away from the body's
                // middle) and drawn with G_CULL_FRONT, which in Rush 2's view culls the side seen from inside (its
                // camera frame flips handedness, so what F3DEX2 calls front is the clockwise side here: G_CULL_BACK left
                // the backs drawn, checked in game). So a lens glows only seen from in front of it: from the bumper
                // camera, which sits behind the car's own lamps, the lenses' backs floated in front of the view on Rush
                // 2049's Magnum and GX-2 (issue 70).
                float e1[3], e2[3], out = 0.0f;
                for (int k = 0; k < 3; k++) e1[k] = t.p[1][k] - t.p[0][k], e2[k] = t.p[2][k] - t.p[0][k];
                const float n[3] = { e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0] };
                for (int k = 0; k < 3; k++) out += n[k] * ((t.p[0][k] + t.p[1][k] + t.p[2][k]) / 3.0f - center[k]);
                const int order[3] = { 0, out < 0.0f ? 2 : 1, out < 0.0f ? 1 : 2 };
                for (int v = 0; v < 3; v++) {
                    for (int w = 0; w < 4; w++) {
                        uint32_t word = (uint32_t)MEM_W(w * 4, (int32_t)t.v[order[v]]);
                        memcpy(lt.vertices.data() + v * 16 + w * 4, &word, 4);
                    }
                }
                lt.texture_w1 = t.texture_w1;
                lt.wrap = t.wrap;
                lt.size0 = t.size0;
                lt.size1 = t.size1;
                found.lens.push_back(lt);
            }
        }
        if (found.lens.size() > 600) found.lens.resize(600);

        if (log_on) {
            std::printf("[car_lights] car %d %s (type %d): %zu triangles, %zu tail texels in %zu groups, %zu head texels in %zu groups, "
                        "%zu lens triangles\n", i, key.c_str(), type, f.tris.size(), tail_points.size(), tails.size(),
                        head_points.size(), heads.size(), found.lens.size());
            std::printf("[car_lights]   %d of %zu brake lists drawn\n", brake_lists, sources.lists.size());
            for (size_t k = 0; k < found.lamps.size(); k++) {
                const Lamp& l = found.lamps[k];
                std::printf("[car_lights]   %s lamp at (%.2f, %.2f, %.2f) size %.2f side %d, %s\n", l.head ? "head" : "tail",
                            l.pos[0], l.pos[1], l.pos[2], l.size, l.side, origin[k].c_str());
            }
            std::fflush(stdout);
        }
        dump(rdram, key, type, f, found, origin);
        return found;
    }

    // ---- drawing a car's lamps ----------------------------------------------------------------------------------------

    constexpr uint32_t car_block = 0x18000;          // a car's share above 16 MB: masks, then three display lists
    constexpr uint32_t masks_size = 0x6000;
    constexpr uint32_t list_size = (car_block - masks_size) / 3;
    uint32_t block_base = 0;

    struct CarState {
        uint64_t key = 0;               // the body it was found for (lists, type, copy base)
        uint64_t content = 0;           // content_key
        bool found = false;
        Found lamps;
        std::vector<uint32_t> mask_at;  // each mask's address
        int buffer = 0;
        uint64_t drawn = ~0ull;         // the lamp states the current list was built for
        bool placed = false;
    };
    CarState state[max_cars];
    int next_car = 0;
    bool models_added = false;

    uint32_t block_of(int car) { return block_base + uint32_t(car) * car_block; }

    void write_cmd(uint8_t* rdram, uint32_t& at, uint32_t w0, uint32_t w1) {
        MEM_W(0, (int32_t)at) = (int32_t)w0;
        MEM_W(4, (int32_t)at) = (int32_t)w1;
        at += 8;
    }

    // The masks into the car's block (once per find).
    void write_masks(uint8_t* rdram, int car, CarState& cs) {
        cs.mask_at.assign(cs.lamps.masks.size(), 0);
        uint32_t at = block_of(car);
        const uint32_t end = at + masks_size;
        for (size_t k = 0; k < cs.lamps.masks.size(); k++) {
            const Mask& m = cs.lamps.masks[k];
            const uint32_t bytes = uint32_t(m.texels.size());
            if (at + bytes > end) break;
            for (uint32_t b = 0; b < bytes; b++) MEM_B(0, (int32_t)(at + b)) = (int8_t)m.texels[b];
            cs.mask_at[k] = at;
            at += (bytes + 7) & ~7u;
        }
    }

    // Bit k of the state: lamp k is on; bit 63: the tail lamps are braking.
    void build_list(uint8_t* rdram, int car, CarState& cs, uint64_t lit) {
        cs.buffer = (cs.buffer + 1) % 3;
        const uint32_t start = block_of(car) + masks_size + uint32_t(cs.buffer) * list_size;
        const uint32_t end = start + list_size;
        uint32_t at = start;
        const bool brake = (lit >> 63) & 1;
        const std::vector<Lamp>& lamps = cs.lamps.lamps;

        // The lens glow.
        bool any_lens = false;
        for (const LensTriangle& t : cs.lamps.lens) any_lens |= ((lit >> t.lamp) & 1) && size_t(t.mask) < cs.mask_at.size() && cs.mask_at[t.mask] != 0;
        if (any_lens) {
            GfxCommand push[4], pop[4];
            gEXPushOtherMode(&push[0]);
            gEXPushCombineMode(&push[1]);
            gEXPushPrimColor(&push[2]);
            gEXPushGeometryMode(&push[3]);
            gEXPopOtherMode(&pop[0]);
            gEXPopCombineMode(&pop[1]);
            gEXPopPrimColor(&pop[2]);
            gEXPopGeometryMode(&pop[3]);
            for (const GfxCommand& c : push) write_cmd(rdram, at, c.values.word0, c.values.word1);
            write_cmd(rdram, at, 0xE7000000, 0);               // G_RDPPIPESYNC
            write_cmd(rdram, at, 0xD9F9F9FF, lens_cull);  // G_CULL_FRONT (see the lens triangles); no lighting or texgen
            write_cmd(rdram, at, 0xE3000A01, 0x00100000);      // cycle type 2-cycle
            write_cmd(rdram, at, 0xE3001001, 0);               // TEXTLUT = NONE
            write_cmd(rdram, at, 0xE3000F00, 0);               // TEXTLOD = TILE
            // Render mode: no fog (G_RM_PASS), then G_RM_ZB_XLU_SURF2: blended by the mask's alpha, depth tested, not
            // written (the model is pushed a little toward the camera, battle_render's push, so it sits over the lens).
            write_cmd(rdram, at, 0xE200001C, 0x0C184A50);
            write_cmd(rdram, at, 0xFCFF97FF, 0xFFFDFE38);      // combiner: PRIM, alpha TEXEL0 x PRIM; then passed on
            for (int lamp = 0; lamp < (int)lamps.size(); lamp++) {
                if (!((lit >> lamp) & 1)) continue;
                bool header = false;
                const LensTriangle* loaded = nullptr;
                for (const LensTriangle& t : cs.lamps.lens) {
                    if (t.lamp != lamp || size_t(t.mask) >= cs.mask_at.size() || cs.mask_at[t.mask] == 0) continue;
                    if (at + 32 * 8 > end) break;
                    if (!header) {
                        header = true;
                        write_cmd(rdram, at, 0xFA000000, lamps[lamp].head ? lens_head : (brake ? lens_brake : lens_dim));
                    }
                    if (loaded == nullptr || t.mask != loaded->mask || t.texture_w1 != loaded->texture_w1 || t.wrap != loaded->wrap ||
                        t.size0 != loaded->size0 || t.size1 != loaded->size1) {
                        const Mask& m = cs.lamps.masks[t.mask];
                        const uint32_t image = cs.mask_at[t.mask];
                        const uint32_t words = uint32_t(m.pitch / 8);
                        const uint32_t texels = uint32_t(m.pitch * m.rows + 1) / 2;   // loaded as 16-bit texels
                        const uint32_t dxt = (2048 + words - 1) / words;
                        write_cmd(rdram, at, 0xFD000000 | (3u << 21) | (2u << 19), image);        // G_SETTIMG IA 16b
                        write_cmd(rdram, at, 0xF5000000 | (3u << 21) | (2u << 19), 0x07000000);  // load tile 7
                        write_cmd(rdram, at, 0xE6000000, 0);                                       // G_RDPLOADSYNC
                        write_cmd(rdram, at, 0xF3000000, 0x07000000 | ((texels - 1) << 12) | (dxt & 0xFFF));
                        write_cmd(rdram, at, 0xE7000000, 0);
                        write_cmd(rdram, at, 0xF5000000 | (3u << 21) | (1u << 19) | (words << 9), t.wrap);  // tile 0, IA8
                        write_cmd(rdram, at, 0xF2000000 | t.size0, t.size1);
                        write_cmd(rdram, at, 0xD7000002, t.texture_w1);    // G_TEXTURE on, tile 0, the part's scale
                        write_cmd(rdram, at, 0xE7000000, 0);
                        loaded = &t;
                    }
                    // The triangle's vertices, copied after the list's commands for it: G_VTX of 3, G_TRI1.
                    const uint32_t verts = at + 16;
                    write_cmd(rdram, at, 0x01003006, verts);        // G_VTX: 3 vertices to slots 0-2
                    write_cmd(rdram, at, 0xDE010000, verts + 48);   // G_DL, no return: on past the vertices
                    for (int w = 0; w < 12; w++) {
                        uint32_t word;
                        memcpy(&word, t.vertices.data() + w * 4, 4);
                        MEM_W(0, (int32_t)(at + uint32_t(w) * 4)) = (int32_t)word;
                    }
                    at += 48;
                    write_cmd(rdram, at, 0x05000204, 0);            // G_TRI1 (0 1 2)
                }
            }
            write_cmd(rdram, at, 0xE7000000, 0);
            write_cmd(rdram, at, 0xD7000000, 0xFFFFFFFF);          // G_TEXTURE off
            for (int c = 3; c >= 0; c--) write_cmd(rdram, at, pop[c].values.word0, pop[c].values.word1);
        }

        // The marker: a vertex a lit lamp (include/rt64_extended_gbi.h gEXSetCarLight), in triangles of three.
        std::vector<int> on;
        for (int lamp = 0; lamp < (int)lamps.size() && on.size() < 30; lamp++) {
            if ((lit >> lamp) & 1) on.push_back(lamp);
        }
        if (!on.empty() && at + (8 + 30) * 8 + 32 * 16 < end) {
            const uint32_t count = uint32_t((on.size() + 2) / 3 * 3);
            GfxCommand c;
            gEXSetCarLight(&c, surface_level, head_reach, tail_reach);
            write_cmd(rdram, at, c.values.word0, c.values.word1);
            const uint32_t verts = at + 16;
            write_cmd(rdram, at, 0x01000000 | (count << 12) | (count << 1), verts);   // G_VTX: count vertices to slots 0-
            write_cmd(rdram, at, 0xDE010000, verts + count * 16);                     // G_DL, no return: past them
            for (uint32_t k = 0; k < count; k++) {
                const uint32_t v = at + k * 16;
                uint32_t rgba = 0;
                int16_t p[3] = {};
                if (k < on.size()) {
                    const Lamp& l = lamps[on[k]];
                    const int strength = l.head || brake ? 255 : tail_dim;
                    const uint8_t* rgb = l.head ? head_rgb : tail_rgb;
                    const uint32_t radius = uint32_t(std::clamp(int(l.size * glow_scale * 32.0f + 0.5f), 1, 127));
                    rgba = (uint32_t(rgb[0] * strength / 255) << 24) | (uint32_t(rgb[1] * strength / 255) << 16) |
                           (uint32_t(rgb[2] * strength / 255) << 8) | (l.head ? 0x80u : 0u) | radius;
                    for (int j = 0; j < 3; j++) p[j] = (int16_t)std::clamp(int(std::lround(l.pos[j] * 16.0f)), -32768, 32767);
                }
                MEM_W(0, (int32_t)v) = (int32_t)((uint32_t(uint16_t(p[0])) << 16) | uint16_t(p[1]));
                MEM_W(4, (int32_t)v) = (int32_t)(uint32_t(uint16_t(p[2])) << 16);
                MEM_W(8, (int32_t)v) = 0;
                MEM_W(12, (int32_t)v) = (int32_t)rgba;
            }
            at += count * 16;
            for (uint32_t k = 0; k < count; k += 3) {
                write_cmd(rdram, at, 0x05000000 | ((k * 2) << 16) | (((k + 1) * 2) << 8) | ((k + 2) * 2), 0);   // G_TRI1
            }
            gEXSetCarLight(&c, 0, 0, 0);
            write_cmd(rdram, at, c.values.word0, c.values.word1);
        }
        write_cmd(rdram, at, 0xDF000000, 0);                       // G_ENDDL

        // The trampoline: into the list above 16 MB and back.
        uint32_t t = trampolines + uint32_t(car) * 32;
        GfxCommand ext;
        gEXSetRDRAMExtended(&ext, 1);
        write_cmd(rdram, t, ext.values.word0, ext.values.word1);
        write_cmd(rdram, t, 0xDE000000, start);
        gEXSetRDRAMExtended(&ext, 0);
        write_cmd(rdram, t, ext.values.word0, ext.values.word1);
        write_cmd(rdram, t, 0xDF000000, 0);
        cs.drawn = lit;
    }

    std::string model_name(int car) { return "CAR_LAMPS" + std::to_string(car); }

    void hide(int i) {
        if (!state[i].placed) return;
        rush2::battle_render::hide(first_slot + i);
        state[i].placed = false;
        if (log_on) std::printf("[car_lights] car %d off\n", i), std::fflush(stdout);
    }

    void hide_all() {
        for (int i = 0; i < max_cars; i++) hide(i);
    }

    // A car's lamps being looked for on a worker (the walk takes a few ms, too long for the game thread), and what
    // it found.
    struct Job {
        uint64_t key = 0;
        std::atomic_bool done = false;
        bool hit = false;
        uint64_t full = 0;
        Found found;
        double ms = 0.0;
        std::string name;
    };
    std::shared_ptr<Job> jobs[max_cars];

    // Car i's lamps found (again) if its body changed: started on a worker, taken up here once it is done.
    void find(uint8_t* rdram, int i) {
        rush2::car2049::BrakeLightSources sources;
        rush2::car2049::brake_light_sources(rdram, i, sources);
        uint32_t lists[6];
        body_lists(rdram, i, lists);
        const int type = MEM_BU(0, (int32_t)(cars + uint32_t(i) * car_size + car_type));
        uint64_t key = (0xCBF29CE484222325ull ^ uint64_t(type + 1)) * 0x100000001B3ull;
        key = (key ^ sources.base) * 0x100000001B3ull;
        for (uint32_t l : lists) key = (key ^ l) * 0x100000001B3ull;
        CarState& cs = state[i];
        if (cs.found && cs.key == key) return;
        if (MEM_H(0, (int32_t)(cars + uint32_t(i) * car_size + car_active)) == 0) return;

        // A job under way: taken up when it is done, if the body is still the one it looked at.
        if (std::shared_ptr<Job> job = jobs[i]) {
            if (!job->done.load()) {
                if (job->key == key) return;
            }
            else {
                jobs[i].reset();
                // Nothing drawn yet (the body is set up after the race's first frames): looked for again.
                if (job->key == key && !job->found.lamps.empty()) {
                    cs.lamps = std::move(job->found);
                    cs.key = key;
                    cs.content = job->full;
                    cs.found = true;
                    cs.drawn = ~0ull;
                    write_masks(rdram, i, cs);
                    if (log_on) {
                        std::printf("[car_lights] car %d %s: %zu lamps, %s in %.2f ms (worker)\n", i, job->name.c_str(),
                                    cs.lamps.lamps.size(), job->hit ? "from the cache" : "worked out", job->ms);
                        std::fflush(stdout);
                    }
                    return;
                }
            }
        }
        if (jobs[i]) return;

        auto job = std::make_shared<Job>();
        job->key = key;
        job->name = car_key(rdram, i, type);
        std::vector<rush2::car_lights::LampSpec> hand;
        bool has_hand = false;
        {
            std::lock_guard lock{ hand_mutex };
            if (auto it = mod_lamps.find(type); it != mod_lamps.end() && !it->second.empty()) hand = it->second, has_hand = true;
            else if (auto t = table.find(job->name); t != table.end()) hand = t->second, has_hand = true;
        }
        jobs[i] = job;
        // The body's lists, textures and vertices stay put while the car is (a change makes a new key, and this
        // job's result is dropped).
        std::array<uint32_t, 6> body;
        std::copy(std::begin(lists), std::end(lists), body.begin());
        std::thread([rdram, i, type, body, sources = std::move(sources), hand = std::move(hand), has_hand, job] {
            const auto t0 = std::chrono::steady_clock::now();
            uint32_t l[6];
            std::copy(body.begin(), body.end(), l);
            uint64_t full = content_key(rdram, type, l, sources);
            for (const auto& s : hand) {
                const float fields[5] = { s.head ? 1.0f : 0.0f, s.at[0], s.at[1], s.at[2], s.size };
                full = rush2::track_cache::hash(reinterpret_cast<const uint8_t*>(fields), sizeof(fields), full);
            }
            job->full = full;
            if (dump_dir == nullptr) {
                std::lock_guard lock{ cache_mutex };
                if (auto it = cache.find(full); it != cache.end()) {
                    job->found = it->second;
                    job->hit = true;
                }
            }
            if (!job->hit) {
                job->found = work_out(rdram, i, type, l, sources, job->name, has_hand ? &hand : nullptr);
                if (!job->found.lamps.empty()) {
                    std::lock_guard lock{ cache_mutex };
                    cache[full] = job->found;
                    cache_dirty = true;
                }
            }
            job->ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
            job->done = true;
        }).detach();
    }

    int panel_level(uint8_t* rdram, int car, int panel) {
        uint32_t bits = (uint32_t)MEM_W(0, (int32_t)(car_states + uint32_t(car) * car_state_size + state_damage));
        uint32_t mask = (uint32_t)MEM_W(0, (int32_t)(panel_masks + panel * 4));
        return int((bits & mask) >> MEM_BU(0, (int32_t)(panel_shifts + panel)));
    }
}

void rush2::car_lights::set_enabled(bool on) {
    enabled = on;
    if (on) {
        load_cache_async();
        load_table_async();
    }
}

void rush2::car_lights::set_braking(int car, bool lit) {
    if (car >= 0 && car < max_cars) braking[car] = lit;
}

void rush2::car_lights::set_mod_lamps(int type, const std::vector<LampSpec>& lamps) {
    std::lock_guard lock{ hand_mutex };
    if (lamps.empty()) mod_lamps.erase(type);
    else mod_lamps[type] = lamps;
}

// pvs_apply_8007C27C, before each view: each car's lamps placed for the views about to be drawn.
extern "C" void rush2_car_lights_view(uint8_t* rdram, recomp_context* ctx) {
    (void)ctx;
    if (!enabled || (int32_t)MEM_W(0, (int32_t)game_state) < 3 || !rush2::track2049::night_race(rdram)) {
        hide_all();
        for (CarState& cs : state) cs.found = false;
        return;
    }
    if (block_base == 0) {
        uint8_t* block = (uint8_t*)recomp::alloc(rdram, car_block * max_cars + 16);
        if (block == nullptr) return;
        block_base = (uint32_t(block - rdram) + 0x80000000u + 15) & ~15u;
    }
    if (!models_added) {
        for (int c = 0; c < max_cars; c++) {
            uint32_t t = trampolines + uint32_t(c) * 32;
            write_cmd(rdram, t, 0xDF000000, 0);
            rush2::battle_render::add_model(model_name(c).c_str(), trampolines + uint32_t(c) * 32);
        }
        models_added = true;
    }
    // One car's lamps looked for (again) a call, if its body changed.
    find(rdram, next_car);
    next_car = (next_car + 1) % max_cars;
    save_cache_async();

    for (int i = 0; i < max_cars; i++) {
        const uint32_t car = cars + uint32_t(i) * car_size;
        CarState& cs = state[i];
        if (!cs.found || MEM_H(0, (int32_t)(car + car_active)) == 0 || MEM_B(0, (int32_t)(car + car_wrecked)) != 0 ||
            rush2::ghost::is_ghost_car(i)) {
            hide(i);
            continue;
        }
        // Which side is the right: the rear right corner's (panel 2) side of the car.
        const bool right_positive = read_f(rdram, car + car_corners + 2 * 12) >= 0.0f;
        uint64_t lit = 0;
        for (int k = 0; k < (int)cs.lamps.lamps.size() && k < 62; k++) {
            const Lamp& l = cs.lamps.lamps[k];
            const bool right = (l.side == 0) == right_positive;
            const int panel = l.head ? (right ? 0 : 1) : (right ? 2 : 3);
            // Out once that corner is damaged at all: collisions only ever raise a panel to level 1 (func_8006B730,
            // func_8009A264 and func_800715E8 stop at slti 2), the level that swaps in its damaged model.
            if (panel_level(rdram, i, panel) >= 1) continue;
            lit |= 1ull << k;
        }
        if (braking[i] || test_on) lit |= 1ull << 63;
        if ((lit & ~(1ull << 63)) == 0) {
            hide(i);
            continue;
        }
        if (lit != cs.drawn) {
            if (log_on && cs.drawn != ~0ull) {
                std::printf("[car_lights] car %d lamps %llx -> %llx\n", i, (unsigned long long)cs.drawn, (unsigned long long)lit);
                std::fflush(stdout);
            }
            build_list(rdram, i, cs, lit);
        }
        float pos[3], m[9];
        rush2::battle::car_body_pose(rdram, i, pos, m);
        // Pushed 0.15 ft toward each camera, so the lens glow sits over the lens it is drawn on.
        rush2::battle_render::place(first_slot + i, model_name(i).c_str(), m, pos, 0xFFFFFFFFu, -1, false, false, -0.15f);
        if (log_on && !cs.placed) std::printf("[car_lights] car %d on\n", i), std::fflush(stdout);
        cs.placed = true;
    }
}
