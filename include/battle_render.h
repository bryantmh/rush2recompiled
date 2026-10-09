#ifndef __BATTLE_RENDER_H__
#define __BATTLE_RENDER_H__

#include <cstdint>

// Draws Rush 2049's battle models (weapons, projectiles, effects, the HUD's models) on any track (src/battle_render.cpp):
// Rush 2049's model files are copied into spare RDRAM as they are, and the models placed here are drawn at the end of
// each view from display lists of their own, so they need no scene nodes or track geometry.
namespace rush2::battle_render {
    constexpr int max_slots = 96;

    // Loads the model files from the Rush 2049 ROM if they aren't (game thread). False without a ROM.
    bool ready(uint8_t* rdram);
    bool has_model(const char* name);
    // Shows `model` in a slot: m's rows are its axes (right, up, forward, with its scale), pos its place in the world.
    // view: the one view it is drawn in, or -1 for all. attached: it rides with that view's camera (a HUD model) and
    // is drawn in view space, so it holds still on screen between game frames (src/interpolation.cpp explains).
    void place(int slot, const char* model, const float m[9], const float pos[3], uint32_t rgba, int view = -1, bool attached = false);
    void hide(int slot);
    // A 2D image of Rush 2049's HUD file (HEALTHBG) as RGBA16 texels in RDRAM. False if it isn't there.
    bool image(uint8_t* rdram, const char* name, uint32_t* address, int* w, int* h);
    void clear();
}

#endif
