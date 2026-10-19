#ifndef __STUNT2049_H__
#define __STUNT2049_H__

#include <cstdint>

// Rush 2049's stunt scoring in place of Rush 2's (src/rush2049/stunt2049.cpp, docs/rush2049_research/stunts.md).
namespace rush2::stunt2049 {
    enum class Scoring : uint32_t { Rush2, Rush2049 };
    // The Stunt Scoring option (Games tab, src/rush2049/wings.cpp). Takes effect at once.
    void set_scoring(Scoring scoring);
    bool enabled();
    // Clears every player's scores and tricks; called when a race is set up.
    void reset();
    // With Rush 2049's scoring, its stunt HUD (icons and text) in place of Rush 2's stunt panel, once Rush 2049's ROM
    // is loaded. Once per drawn HUD, after the widgets (src/hud.cpp rush2_hud_draw_end).
    void draw_hud(uint8_t* rdram);
}

#endif
