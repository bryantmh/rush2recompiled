#ifndef __RUSH2_HOOKS_H__
#define __RUSH2_HOOKS_H__

// Declarations for runtime functions called from hooks inserted into recompiled code (see us.toml).

#include <stdint.h>

#include "recomp.h"

#ifdef __cplusplus
extern "C" {
#endif

void load_overlays(uint32_t rom, int32_t ram_addr, uint32_t size);

// Lets other game threads run (and processes pending RCP/VI events) for up to 1ms.
void yield_self_1ms(uint8_t* rdram);

// Called by the game's pak thread when a Controller Pak initializes, so the port also rumbles (src/pak.cpp).
void rush2_enable_pak_rumble(uint8_t* rdram, recomp_context* ctx);

// HUD edge anchoring (src/hud.cpp).
void rush2_hud_build_begin(uint8_t* rdram, recomp_context* ctx);
void rush2_hud_build_end(uint8_t* rdram, recomp_context* ctx);
void rush2_hud_widget_created(uint8_t* rdram, recomp_context* ctx);
void rush2_hud_draw_begin(uint8_t* rdram, recomp_context* ctx);
void rush2_hud_draw_widget(uint8_t* rdram, recomp_context* ctx);
void rush2_hud_draw_end(uint8_t* rdram, recomp_context* ctx);
void rush2_hud_print(uint8_t* rdram, recomp_context* ctx);
void rush2_hud_laps_begin(uint8_t* rdram, recomp_context* ctx);
void rush2_hud_laps_end(uint8_t* rdram, recomp_context* ctx);

// Frame interpolation matrix group tagging (src/interpolation.cpp).
void rush2_interp_view_begin(uint8_t* rdram, recomp_context* ctx);
void rush2_interp_projection(uint8_t* rdram, recomp_context* ctx);
void rush2_interp_root_modelview(uint8_t* rdram, recomp_context* ctx);
void rush2_interp_poly(uint8_t* rdram, recomp_context* ctx);
void rush2_interp_view_end(uint8_t* rdram, recomp_context* ctx);
void rush2_interp_level_enter(uint8_t* rdram, recomp_context* ctx);
void rush2_interp_level_exit(uint8_t* rdram, recomp_context* ctx);
void rush2_interp_node_begin(uint8_t* rdram, recomp_context* ctx);
void rush2_interp_node_matrix(uint8_t* rdram, recomp_context* ctx);
void rush2_interp_node_pop(uint8_t* rdram, recomp_context* ctx);

// Full-screen frame clear and menu background for widescreen (src/widescreen.cpp).
void rush2_frame_clear_begin(uint8_t* rdram, recomp_context* ctx);
void rush2_frame_clear_end(uint8_t* rdram, recomp_context* ctx);
void rush2_model_draw(uint8_t* rdram, recomp_context* ctx);

// Level of detail override (src/lod.cpp).
void rush2_lod_select(uint8_t* rdram, recomp_context* ctx);

// Applies the Cheats tab's settings once per frame (src/cheats.cpp).
void rush2_cheats_frame(uint8_t* rdram, recomp_context* ctx);

// High-resolution font tile clamping around the 2D image loader (src/fonts.cpp).
void rush2_font_load_begin(uint8_t* rdram, recomp_context* ctx);
void rush2_font_load_end(uint8_t* rdram, recomp_context* ctx);

// WINGS row in the Controller Setup screen (src/wings_menu.cpp).
void rush2_wings_menu_widgets(uint8_t* rdram, recomp_context* ctx);
void rush2_wings_menu_defaults(uint8_t* rdram, recomp_context* ctx);
int rush2_wings_menu_edit(uint8_t* rdram, recomp_context* ctx);
void rush2_wings_menu_conflicts(uint8_t* rdram, recomp_context* ctx);
void rush2_wings_menu_icon_conflict(uint8_t* rdram, recomp_context* ctx);
void rush2_wings_menu_icon_binding(uint8_t* rdram, recomp_context* ctx);
void rush2_wings_menu_labels(uint8_t* rdram, recomp_context* ctx);
void rush2_wings_menu_cursor(uint8_t* rdram, recomp_context* ctx);
void rush2_wings_menu_label_row(uint8_t* rdram, recomp_context* ctx);

// Rush 2049 wings physics and drawing (src/wings_state.cpp, src/wings_render.cpp).
void rush2_wings_torque(uint8_t* rdram, recomp_context* ctx);
void rush2_wings_save_position(uint8_t* rdram, recomp_context* ctx);
void rush2_wings_gravity(uint8_t* rdram, recomp_context* ctx);
void rush2_wings_drag(uint8_t* rdram, recomp_context* ctx);
void rush2_wings_car_init(uint8_t* rdram, recomp_context* ctx);
void rush2_wings_model_draw(uint8_t* rdram, recomp_context* ctx);

#ifdef __cplusplus
}
#endif

#endif
