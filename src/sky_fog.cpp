// Sky Fog (Settings > Graphics > Effects): blends the race's fog into the sky the way haze does.
//
// The game fogs the world by distance but draws the sky (Rush 2's cloud dome, the Las Vegas, stunt arena and Rush 2049
// sky models, the clear color) without fog, so on a clear day the fogged skyline stands out as a pale band against a
// blue sky. With Sky Fog on, RT64 hazes the sky by the fog a ray at each angle above the horizon passes through
// (SkyFogPS.hlsl, the sky fog pass in rt64_framebuffer_renderer.cpp, docs/graphics_effects.md#sky-fog): fully the fog
// color at the horizon, where the world's fog is complete at the far plane too, clearing with height.
//
// The model is an exponential height fog: density sigma at the camera's height, falling by e every H above it. A ray
// at elevation e passes through optical depth sigma * H / sin e, so the sky keeps exp(-haze / sin e) of its light with
// haze = sigma * H. sigma comes from the view's own fog, so the sky matches the world and follows the FOG option, fog
// zones and the draw distance:
// - func_8007C624 sets the fog with gSPFogPosition(min, max) from the view's +0x4C / +0x4E (0-1000), so a point at
//   distance d gets fog (D - min) / (max - min), D = 500 * (z / w + 1) and z / w = a - b / d from guPerspective
//   (a = (f + n) / (f - n), b = 2fn / (f - n)). The fog is linear in 1 / d between its start (D = min) and end
//   (D = max); with the draw distance factor k both ends are k times farther (src/draw_distance.cpp).
// - sigma = ln 2 / the distance where the game's fog is half: the fog of a uniform medium is 1 - exp(-sigma d), and
//   this one matches the game's at its middle and at both extremes (none close up, all of it far away).
// - H = 200 ft, well under the 900 ft cloud lid of Rush 2's dome: a shallow haze layer under the clouds. One player
//   with FOG 2 (fog from about 260 ft to 1500 ft, draw distance Original) gives haze 0.31: the zenith keeps 73% of its
//   color, 10 degrees up 17%, 2 degrees under 1%.
//
// Only in a race (game states 3, 6, 9 and 10, docs/menus.md#game-state): the car and track selects' 3D views draw over
// 2D backgrounds, which would be hazed too. Every view emits its haze (0 when off) with gEXSetSkyFog, so the setting
// takes effect on the next frame. A view without fog (fog_removed, below) gets haze 0 and keeps its sky as it is.
//
// The sky's draws are marked with gEXSetSkyDraw: Rush 2's cloud dome pieces (rush2_interp_poly_end, which redraws
// them) and the sky model (model_draw below). RT64 hazes right after the view's last sky draw, every pixel then still
// at the depth clear, so see-through scenery drawn later without depth writes (Marina's boat sails) is left alone. A
// view with no sky draw shows the clear color, which the game sets to the fog color already.

#include <algorithm>
#include <atomic>
#include <cmath>

#include "recomp.h"
#include "rush2.h"
#include "rush2_hooks.h"
#include "players4.h"
#include "scene_nodes.h"
#include "track2049.h"
#include "rt64_extended_gbi.h"

extern "C" uint32_t rush2_interp_side_dl(uint8_t* rdram, const GfxCommand* cmds, uint32_t count);

namespace {
    constexpr uint32_t game_state = 0x8010C0D0;
    constexpr double scale_height = 200.0 * 16.0;  // H, in view units (1/16 ft).
    // Per-view sky model transforms (sky_setup_800A45A8, moved for four views, docs/players4.md#sky-in-split-screen):
    // the sky model's scene node points at its view's (node +4).
    constexpr uint32_t sky_transforms = 0x80125B68;
    constexpr uint32_t fog_option = 0x800D1FA8;    // FOG, 1-4 (race_graphics_setting_byte).

    constexpr uint32_t views = 0x802401F0;           // Player views, 0x54 each (moved from 0x80111888).
    constexpr uint32_t view_size = 0x54;

    std::atomic<bool> on_flag = false;

    // A view's index from its address (4 = not a player view).
    uint32_t view_index(uint8_t*, uint32_t view) {
        return view >= views && view < views + 4 * view_size ? (view - views) / view_size : 4;
    }
    uint16_t pending_haze = 0;

    bool in_race(uint8_t* rdram) {
        switch (MEM_W(0, (int32_t)game_state)) {
            case 3:
            case 6:
            case 9:
            case 10:
                return true;
            default:
                return false;
        }
    }
}

void rush2::sky_fog::set_enabled(bool on) {
    on_flag.store(on, std::memory_order_relaxed);
}

bool rush2::sky_fog::enabled() {
    return on_flag.load(std::memory_order_relaxed);
}

// model_draw_8007AA48 at 0x8007AF84 (rush2_model_draw): $s7 = node, $fp = the model's display list. The sky model's
// list is called between gEXSetSkyDraw(1) and (0).
extern "C" void rush2_sky_fog_model_draw(uint8_t* rdram, recomp_context* ctx) {
    if (!rush2::sky_fog::enabled() || !in_race(rdram)) {
        return;
    }
    const uint32_t node = (uint32_t)ctx->r23;
    if (node < rush2::scene_nodes::table || node >= rush2::scene_nodes::table_end) {
        return;
    }
    const uint32_t transform = (uint32_t)MEM_W(4, (int32_t)node);
    const uint32_t skies = rush2::players4::moved(sky_transforms);
    bool sky = false;
    for (uint32_t v = 0; v < 4 && !sky; v++) {
        const uint32_t t = (uint32_t)MEM_W((int32_t)(v * 4), (int32_t)skies);
        sky = t != 0 && t == transform;
    }
    if (!sky) {
        return;
    }
    GfxCommand cmds[3];
    gEXSetSkyDraw(&cmds[0], 1);
    cmds[1].values.word0 = 0xDE000000u;  // G_DL (call)
    cmds[1].values.word1 = (uint32_t)ctx->r30;
    gEXSetSkyDraw(&cmds[2], 0);
    ctx->r30 = (uint64_t)(int64_t)(int32_t)rush2_interp_side_dl(rdram, cmds, 3);
}

// FOG at its lowest (1) is no fog on a converted course with a sky model (a Rush 2049 track or arena, an arcade or
// custom track with a SKY mesh or a stock sky), whatever Sky Fog is set to: Rush 2049's own lowest fog level is no fog
// (func_800AB18C, its twin of race_fog_zone_setup_80081074, returns at level 0 and leaves every race view at fog range
// 1000 / 999, docs/rush2049_research/race.md#fog), and the other courses with a sky model follow it, as the fogged
// skyline would otherwise stand out against the sky. Stock Rush 2 courses keep Rush 2's fog, Las Vegas and the stunt
// arena (sky models) included, and so do courses with Rush 2's cloud dome (SF Rush, California Speed): Rush 2 fogs
// from about 477 ft at FOG 1. A view has a sky model when sky_setup_800A45A8 gave it a transform (moved 0x80125B68;
// views 2 and 3 draw views 0 and 1's, docs/players4.md#sky-in-split-screen). Done on the fog RT64 draws (fog factors 0,
// rush2_draw_distance_projection) so the game's own fog values, which the fog zones read, are left alone.
bool rush2::sky_fog::fog_removed(uint8_t* rdram, uint32_t view_address) {
    const uint32_t view = view_index(rdram, view_address);
    if (!in_race(rdram) || view >= 4 || MEM_B(0, (int32_t)fog_option) != 1 || !rush2::track2049::converted_race(rdram)) {
        return false;
    }
    return MEM_W((int32_t)((view & 1) * 4), (int32_t)rush2::players4::moved(sky_transforms)) != 0;
}

void rush2::sky_fog::view_setup(uint8_t* rdram, uint32_t view, double near16, double far16) {
    pending_haze = 0;
    if (!enabled() || !in_race(rdram) || near16 <= 0.0 || far16 <= near16 || fog_removed(rdram, view)) {
        return;
    }

    const double fog_min = MEM_HU(0x4C, (int32_t)view);
    const double fog_max = MEM_HU(0x4E, (int32_t)view);
    if (fog_max <= fog_min) {
        return;
    }

    // 1 / distance at the fog's start and end: a + 1 - b / d = D / 500.
    const double a = (far16 + near16) / (far16 - near16);
    const double b = 2.0 * far16 * near16 / (far16 - near16);
    const double inv_start = (a + 1.0 - fog_min / 500.0) / b;
    const double inv_end = std::max((a + 1.0 - fog_max / 500.0) / b, 0.0);
    if (inv_start <= 0.0) {
        return;  // No fog this side of infinity.
    }

    const double scale = std::max((double)rush2::draw_distance(), 0.01);
    const double inv_half = (inv_start + inv_end) / 2.0 / scale;
    const double haze = std::log(2.0) * inv_half * scale_height;
    pending_haze = (uint16_t)std::clamp(std::lround(haze * 1000.0), 1L, 65535L);
}

uint16_t rush2::sky_fog::take_haze() {
    uint16_t haze = pending_haze;
    pending_haze = 0;
    return haze;
}
