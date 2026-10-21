// The Games tab's Arcade Checkpoints option (include/arcade2049.h, docs/arcade2049.md#arcade-checkpoints): the Rush 2049
// arcade's CHECKPOINT banner (texture MG_CHECK5, a 256 x 64 quad hung across the road on arcade tracks 1-5) over each
// checkpoint of the Rush 2049 race tracks, which the N64 game marks with nothing.
//
// The banner comes from the Rush 2049 Arcade Tracks mod (checkpoint.png in rush2049_arcade.rtz, written by Build and
// Install), so a copy of that mod brings it to an install without the arcade data. The gates are the race's own: Rush
// 2's copy of the path header at 0x8010BCE8 (s16 finish checkpoint at +4, count at +8, 0x50-byte checkpoints from +0xC:
// f32 center on the road, f32 plane normal with y 0; docs/rush2049_research/race.md), so backward races get theirs.
// Every checkpoint but the finish line (which has its own sign) gets one, centered on the AI lanes where they cross the
// gate and standing along the up vector of the ground there (Rush 2's ground query), so it leans with a sloped or
// banked road. It is drawn by rush2::battle_render as a model of its own: a 64 x 16 RGBA16 stand-in of the image,
// drawn at full size as a mod texture (rush2::derive), on a 64 x 16 ft quad whose bottom is 22 ft above the ground,
// as the arcade's hang (8-18 ft tall, 40-130 ft wide, about 25 ft up). It has a second face just behind the first,
// mirrored, so it reads the right way round from both sides.

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <thread>
#include <unordered_map>

#include "librecomp/config.hpp"
#include "librecomp/mods.hpp"
#include "recomp.h"

#include "arcade2049.h"
#include "arcade_checkpoints.h"
#include "battle.h"
#include "battle_render.h"
#include "options_page.h"
#include "texture_derive.h"
#include "texture_upscale.h"
#include "track1.h"
#include "track2049.h"
#include "track_cache.h"
#include "wings_internal.h"

namespace {
    const std::string option_id = "arcade_checkpoints";
    std::atomic_bool option_on = false;

    constexpr const char* model_name = "ARCADE_CHECKPOINT";
    constexpr int first_slot = 176;               // to 185 (battle_render.h)
    constexpr int max_gates = 10;

    constexpr uint32_t track_id = 0x8010C3F0;     // s8, the track raced (2049 tracks race in the host slot)
    constexpr uint32_t game_state = 0x8010C0D0;   // s32, docs/menus.md
    constexpr uint32_t path_header = 0x8010BCE8;  // Rush 2's copy of the raced path's header

    // Spare RDRAM (the list in src/controls_menu.cpp): the stand-in's texels, the quad's vertices, its display list.
    constexpr uint32_t texels_at = 0x80E1E000;    // 64 x 16 RGBA16, 2048 bytes
    constexpr uint32_t vertices_at = 0x80E1E800;  // 8 x 16 bytes
    constexpr uint32_t dl_at = 0x80E1E900;        // to 0x80E1EA00
    constexpr int stand_w = 64, stand_h = 16;

    // Banner size and height above the gate, in feet.
    constexpr float half_width = 32.0f, bottom = 22.0f, top = 38.0f, back_gap = 0.25f;

    std::mutex mutex;
    std::vector<uint8_t> stand_in;                // big-endian RGBA16 texels, empty until the image is read
    std::atomic_bool loading = false;
    std::atomic_int image_generation = 0;         // bumped by each read, so the frame writes the new texels
    int written_generation = -1;
    bool shown = false;

    // Reads checkpoint.png from the arcade tracks mod (the game's .rtz or an old Python install's loose folder) on a worker,
    // and publishes the full image for the stand-in.
    void load_image() {
        if (loading.exchange(true)) return;
        std::thread([]() {
            std::filesystem::path mods = recomp::mods::get_mods_directory();
            std::vector<uint8_t> png;
            rush2::upscale::ZipReader zip(mods / (std::string(rush2::arcade2049::track_mod_id) + ".rtz"));
            if (!zip.is_open() || !zip.read("checkpoint.png", png)) {
                png = rush2::upscale::read_file(mods / rush2::arcade2049::track_mod_id / "checkpoint.png");
            }
            rush2::upscale::Image image;
            std::vector<uint8_t> texels;
            if (!png.empty() && rush2::upscale::decode_image(png, image) && !image.empty()) {
                rush2::upscale::Image small = rush2::upscale::shrink(image, stand_w, stand_h);
                texels.resize(size_t(stand_w) * stand_h * 2);
                for (size_t i = 0; i < size_t(stand_w) * stand_h; i++) {
                    const uint8_t* q = &small.rgba[i * 4];
                    uint16_t c = uint16_t(((q[0] >> 3) << 11) | ((q[1] >> 3) << 6) | ((q[2] >> 3) << 1) | 1);
                    texels[i * 2] = uint8_t(c >> 8);
                    texels[i * 2 + 1] = uint8_t(c);
                }
                std::unordered_map<uint64_t, rush2::derive::ModTexture> textures;
                rush2::derive::ModTexture mt;
                uint64_t key = rush2::upscale::rgba16_key(texels, stand_w, stand_h);
                mt.id = rush2::upscale::content_key(image);
                mt.load = [image](rush2::upscale::Image& out, rush2::upscale::Image&) {
                    out = image;
                    return true;
                };
                textures[key] = mt;
                rush2::derive::set_mod_textures("arcade_checkpoints", std::move(textures));
            }
            else {
                printf("[arcade2049] No checkpoint banner in the %s mod: Build and Install it again\n",
                       rush2::arcade2049::track_mod_id);
            }
            {
                std::lock_guard lock{ mutex };
                stand_in = std::move(texels);
            }
            image_generation++;
            loading = false;
        }).detach();
    }

    // The banner's display list, vertices and texels (game thread).
    void write_model(uint8_t* rdram, const std::vector<uint8_t>& texels) {
        for (size_t i = 0; i + 3 < texels.size(); i += 4) {
            MEM_W(0, (int32_t)(texels_at + i)) =
                (int32_t)((uint32_t(texels[i]) << 24) | (uint32_t(texels[i + 1]) << 16) | (uint32_t(texels[i + 2]) << 8) | texels[i + 3]);
        }
        // Vertices (1/16 ft): s16 x, y, z, u16 0, s16 s, t (10.5), u8 r, g, b, a. The front face is seen from +z, the back
        // one from -z with its s reversed.
        const int16_t x = int16_t(half_width * 16), y0 = int16_t(bottom * 16), y1 = int16_t(top * 16), zb = int16_t(-back_gap * 16);
        const int16_t s1 = stand_w * 32, t1 = stand_h * 32;
        const int16_t v[8][5] = {
            { int16_t(-x), y0, 0, 0, t1 }, { x, y0, 0, s1, t1 }, { x, y1, 0, s1, 0 }, { int16_t(-x), y1, 0, 0, 0 },
            { x, y0, zb, 0, t1 }, { int16_t(-x), y0, zb, s1, t1 }, { int16_t(-x), y1, zb, s1, 0 }, { x, y1, zb, 0, 0 },
        };
        for (uint32_t i = 0; i < 8; i++) {
            uint32_t at = vertices_at + i * 16;
            MEM_W(0, (int32_t)at) = (int32_t)((uint32_t(uint16_t(v[i][0])) << 16) | uint16_t(v[i][1]));
            MEM_W(0, (int32_t)(at + 4)) = (int32_t)(uint32_t(uint16_t(v[i][2])) << 16);
            MEM_W(0, (int32_t)(at + 8)) = (int32_t)((uint32_t(uint16_t(v[i][3])) << 16) | uint16_t(v[i][4]));
            MEM_W(0, (int32_t)(at + 12)) = (int32_t)0xFFFFFFFF;
        }
        // No fog: the fog in place when rush2::battle_render draws (the end of the view) isn't the scenery's, and with
        // G_RM_FOG_SHADE_A the banner came out in the fog color, the sky's, from about 100 ft away.
        const uint32_t dl[][2] = {
            { 0xE7000000, 0x00000000 },        // G_RDPPIPESYNC
            { 0xE3001001, 0x00000000 },        // TEXTLUT = NONE
            { 0xE3000F00, 0x00000000 },        // TEXTLOD = TILE
            { 0xE200001C, 0x0C192230 },        // render mode: no fog (see below), then z-buffered opaque surface
            { 0xFC127FFF, 0xFFFFF238 },        // combiner: texel 0 x shade
            { 0xFD100000, texels_at },         // G_SETTIMG RGBA16
            { 0xF5100000, 0x07000000 },        // load tile 7
            { 0xE6000000, 0x00000000 },        // G_RDPLOADSYNC
            { 0xF3000000, 0x073FF080 },        // G_LOADBLOCK 1024 texels, dxt 128 (a 64-texel row is 16 words)
            { 0xE7000000, 0x00000000 },
            { 0xF5102000, 0x00080200 },        // tile 0: RGBA16, 16 words a row, clamped in s and t
            { 0xF2000000, 0x000FC03C },        // tile size 64 x 16
            { 0x01008010, vertices_at },       // G_VTX 8 from 0
            { 0x06000204, 0x00000406 },        // G_TRI2 front (0 1 2) (0 2 3)
            { 0x06080A0C, 0x00080C0E },        // G_TRI2 back (4 5 6) (4 6 7)
            { 0xDF000000, 0x00000000 },        // G_ENDDL
        };
        for (uint32_t i = 0; i < sizeof(dl) / sizeof(dl[0]); i++) {
            MEM_W(0, (int32_t)(dl_at + i * 8)) = (int32_t)dl[i][0];
            MEM_W(0, (int32_t)(dl_at + i * 8 + 4)) = (int32_t)dl[i][1];
        }
    }

    void hide_all() {
        for (int i = 0; i < max_gates; i++) rush2::battle_render::hide(first_slot + i);
        shown = false;
    }

    float read_f32(uint8_t* rdram, uint32_t at) {
        uint32_t w = (uint32_t)MEM_W(0, (int32_t)at);
        float f;
        std::memcpy(&f, &w, 4);
        return f;
    }

    // Each gate's banner: rows right, up, forward and its place (the ground under the middle of the AI lanes).
    struct Placement {
        bool on = false;
        float m[9] = {}, pos[3] = {};
    };
    Placement placements[max_gates];
    uint64_t placed_key = 0;

    // A banner per checkpoint but the start / finish line (it has its own sign): across the middle of the AI lanes where
    // they cross the gate's line, standing on the ground there along its up vector, so it leans with the road's slope
    // and banking, and facing along the gate's direction.
    void compute_placements(uint8_t* rdram, recomp_context* ctx) {
        int count = std::clamp<int>((int16_t)MEM_H(0, (int32_t)(path_header + 8)), 0, max_gates);
        int finish = (int16_t)MEM_H(0, (int32_t)(path_header + 4));
        // Test aid: RUSH2_TEST_ARCADE_CHECKPOINTS=finish hangs one over the start line too, in view at the start.
        static const bool test_finish = [] {
            const char* v = std::getenv("RUSH2_TEST_ARCADE_CHECKPOINTS");
            return v != nullptr && std::strcmp(v, "finish") == 0;
        }();
        if (test_finish) finish = -1;
        for (int i = 0; i < max_gates; i++) {
            Placement& p = placements[i];
            p.on = false;
            if (i >= count || i == finish) continue;
            uint32_t cp = path_header + 0xC + uint32_t(i) * 0x50;
            float base[3], f[3] = { read_f32(rdram, cp + 0xC), 0.0f, read_f32(rdram, cp + 0x14) };
            if (!rush2::battle::checkpoint_lane_middle(rdram, i, base)) {
                for (int c = 0; c < 3; c++) base[c] = read_f32(rdram, cp + c * 4);
            }
            float up[3] = { 0.0f, 1.0f, 0.0f }, ground_y = base[1], n[3];
            float probe[3] = { base[0], base[1] + 2.0f, base[2] };
            if (ctx != nullptr && rush2::battle::ground_under(rdram, ctx, probe, ground_y, n)) {
                float len = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
                if (len > 0.001f && n[1] / len > 0.3f) {
                    for (int c = 0; c < 3; c++) up[c] = n[c] / len;
                }
            }
            // Forward: the gate's direction with its part along up taken out.
            float d = f[0] * up[0] + f[2] * up[2];
            for (int c = 0; c < 3; c++) f[c] -= up[c] * d;
            float len = std::sqrt(f[0] * f[0] + f[1] * f[1] + f[2] * f[2]);
            if (!(len > 0.001f)) continue;
            for (float& v : f) v /= len;
            float r[3] = { up[1] * f[2] - up[2] * f[1], up[2] * f[0] - up[0] * f[2], up[0] * f[1] - up[1] * f[0] };
            const float m[9] = { r[0], r[1], r[2], up[0], up[1], up[2], f[0], f[1], f[2] };
            std::copy(m, m + 9, p.m);
            p.pos[0] = base[0];
            p.pos[1] = ground_y;
            p.pos[2] = base[2];
            p.on = true;
        }
    }
}

void rush2::arcade2049::init_checkpoints_config(recomp::config::Config& config) {
    config.add_bool_option(
        option_id,
        "Arcade Checkpoints",
        "Hangs the arcade game's CHECKPOINT banner over every checkpoint on the Rush 2049 tracks, which the N64 game "
        "leaves unmarked. Needs the Rush 2049 Arcade Tracks mod (Build and Install above, or a copy of the mod from "
        "another install). Takes effect at once.",
        false
    );
    config.add_option_change_callback(option_id,
        [](recomp::config::ConfigValueVariant cur_value, recomp::config::ConfigValueVariant, recomp::config::OptionChangeContext) {
            option_on = std::get<bool>(cur_value);
            if (option_on) load_image();
        });
}

void rush2::arcade2049::load_checkpoints_config() {
    option_on = std::get<bool>(rush2::wings::games_config().get_option_value(option_id));
    if (option_on) load_image();
}

void rush2::arcade2049::add_checkpoints_option(rush2::ui::OptionsPage* page) {
    page->add_option(rush2::wings::games_config(), option_id);
}

void rush2::arcade2049::checkpoints_changed() {
    if (option_on) load_image();
}

void rush2::arcade2049::checkpoints_frame(uint8_t* rdram, recomp_context* ctx) {
    int state = MEM_W(0, (int32_t)game_state);
    bool racing = state == 3 || state == 4 || state == 5 || state == 9 || state == 0xA;
    int k = rush2::track2049::race_track();
    bool on_2049 = (int8_t)MEM_B(0, (int32_t)track_id) == rush2::track2049::host_slot && rush2::track1::race_track() == 0 &&
                   k >= 1 && k <= rush2::track2049::track_count;
    std::vector<uint8_t> texels;
    int generation = image_generation;
    if (option_on && racing && on_2049) {
        std::lock_guard lock{ mutex };
        texels = stand_in;
    }
    if (texels.empty() || !rush2::battle_render::ready(rdram)) {
        if (shown) hide_all();
        return;
    }
    if (written_generation != generation || !shown) {
        write_model(rdram, texels);
        rush2::battle_render::add_model(model_name, dl_at);
        written_generation = generation;
    }
    // The banners' places, worked out again when the race's checkpoints change (a new race or direction).
    uint64_t key = 0x43504B;
    for (uint32_t o = 0; o < 0xC + max_gates * 0x50; o += 4) {
        uint32_t w = (uint32_t)MEM_W(0, (int32_t)(path_header + o));
        key = rush2::track_cache::hash(reinterpret_cast<const uint8_t*>(&w), 4, key);
    }
    if (key != placed_key) {
        placed_key = key;
        compute_placements(rdram, ctx);
    }
    for (int i = 0; i < max_gates; i++) {
        const Placement& p = placements[i];
        if (p.on) rush2::battle_render::place(first_slot + i, model_name, p.m, p.pos, 0xFFFFFFFF);
        else rush2::battle_render::hide(first_slot + i);
    }
    shown = true;
}
