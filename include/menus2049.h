#ifndef __MENUS2049_H__
#define __MENUS2049_H__

#include <cstdint>

#include "recomp.h"

// Rush 2049 Menus (src/rush2049/menus2049.cpp; docs/rush2049_research/menus.md section 11): the front end's text in
// Rush 2049's fonts and colors, Rush 2049's menu background (its swirl layer and circuit board) in place of the
// scrolling "RUSH2" mesh, and Rush 2049's menu buttons behind the rows of the menu lists, flipping over to the
// yellow side for the row under the cursor. Option "rush2049_menus" in the Games tab (games.json), off by default.
namespace rush2::menus2049 {
    void set_option(bool on);
    // The Rush 2049 source changed: the art is built again from the new one.
    void source_changed();

    // Hooks from other modules.
    // rush2_hud_print (src/hud.cpp): text_print_string_800734E0 entry, before the string is drawn.
    void before_print(uint8_t* rdram, recomp_context* ctx);
    // rush2_model_draw (src/widescreen.cpp): the display list a model is about to be drawn with. Returns the list to
    // draw instead (an empty one for Rush 2's menu background while Rush 2049's is shown), else dl.
    uint32_t model_list(uint8_t* rdram, uint32_t dl, bool menu_background);
}

#endif
