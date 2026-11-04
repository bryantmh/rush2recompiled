#ifndef __MENUS2049_H__
#define __MENUS2049_H__

#include <cstdint>

#include "recomp.h"

// Rush 2049 Menus (src/rush2049/menus2049.cpp; docs/rush2049_research/menus.md section 11): the front end's text in
// Rush 2049's fonts and colors, Rush 2049's menu background (its swirl layer and circuit board) in place of the
// scrolling "RUSH2" mesh, and Rush 2049's menu buttons behind the rows of the menu lists, flipping over to the
// yellow side for the row under the cursor. With Style and Layout, the screens' header plates also give way to
// 2049's title button and the home screen (Start Game) is laid out like 2049's. Option "rush2049_menus" in the Games
// tab (games.json), off by default.
namespace rush2::menus2049 {
    enum class Mode : uint32_t { Off = 0, Style = 1, Layout = 2 };
    void set_option(Mode mode);
    // The Rush 2049 source changed: the art is built again from the new one.
    void source_changed();

    // Hooks from other modules.
    // rush2_hud_print (src/hud.cpp): text_print_string_800734E0 entry, before the string is drawn.
    void before_print(uint8_t* rdram, recomp_context* ctx);
    // rush2_model_draw (src/widescreen.cpp): the display list a model is about to be drawn with. Returns the list to
    // draw instead (an empty one for Rush 2's menu background while Rush 2049's is shown), else dl.
    uint32_t model_list(uint8_t* rdram, uint32_t dl, bool menu_background);
    // rush2_hud_draw_widget (src/hud.cpp): a widget is about to be drawn; its layout entry's image name and callback
    // (hud.cpp's slot identity) and whether the game hides it. True to skip its draw this frame.
    bool skip_widget(uint8_t* rdram, const char* name, uint32_t callback, bool hidden);
    // rush2_hud_draw_end (src/hud.cpp): every widget of the frame is drawn; the rows' buttons are laid out and drawn
    // and the home screen's rows printed.
    void list_end(uint8_t* rdram, recomp_context* ctx);
    // rush2_hud_2d_list_end (src/hud.cpp): the frame's 2D display list is complete (buttons list_end didn't draw).
    void finish_buttons(uint8_t* rdram);
}

#endif
