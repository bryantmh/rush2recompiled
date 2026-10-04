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

// Scales the car's throttle and brake by trigger pressure (src/input.cpp).
void rush2_analog_pedals(uint8_t* rdram, recomp_context* ctx);

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

// Player view scissors, full-screen frame clear and menu background for widescreen (src/widescreen.cpp).
void rush2_view_scissor(uint8_t* rdram, recomp_context* ctx);
void rush2_view_scissor_written(uint8_t* rdram, recomp_context* ctx);
void rush2_frame_clear_begin(uint8_t* rdram, recomp_context* ctx);
void rush2_frame_clear_end(uint8_t* rdram, recomp_context* ctx);
void rush2_model_draw(uint8_t* rdram, recomp_context* ctx);

// Level of detail override (src/lod.cpp).
void rush2_lod_select(uint8_t* rdram, recomp_context* ctx);

// Applies the Cheats tab's settings once per frame (src/cheats.cpp).
void rush2_cheats_frame(uint8_t* rdram, recomp_context* ctx);
void rush2_cheats_car_list(uint8_t* rdram, recomp_context* ctx);

// High-resolution font tile clamping around the 2D image loader (src/fonts.cpp).
void rush2_font_load_begin(uint8_t* rdram, recomp_context* ctx);
void rush2_font_load_end(uint8_t* rdram, recomp_context* ctx);

// Replacement asset files and the relocated game heap (src/assets.cpp).
int rush2_asset_decompress(uint8_t* rdram, recomp_context* ctx);
void rush2_heap_init(uint8_t* rdram, recomp_context* ctx);

// Rush 2049 tracks raced in a borrowed track slot (src/track2049.cpp).
void rush2_track49_load(uint8_t* rdram, recomp_context* ctx);
void rush2_track49_pvs(uint8_t* rdram, recomp_context* ctx);
void rush2_track49_sky(uint8_t* rdram, recomp_context* ctx);
void rush2_track49_sky_players(uint8_t* rdram, recomp_context* ctx);
void rush2_track49_select_init(uint8_t* rdram, recomp_context* ctx);
void rush2_track49_select_count(uint8_t* rdram, recomp_context* ctx);
int rush2_track49_select_available(uint8_t* rdram, recomp_context* ctx);
void rush2_track49_select_wrap(uint8_t* rdram, recomp_context* ctx);
void rush2_track49_select_save_p1(uint8_t* rdram, recomp_context* ctx);
void rush2_track49_select_save_p2(uint8_t* rdram, recomp_context* ctx);
int rush2_track49_keys(uint8_t* rdram, recomp_context* ctx);
void rush2_track49_race_start(uint8_t* rdram, recomp_context* ctx);
void rush2_track49_movers_tick(uint8_t* rdram, recomp_context* ctx);
void rush2_track49_music(uint8_t* rdram, recomp_context* ctx);
void rush2_car49_list(uint8_t* rdram, recomp_context* ctx);
void rush2_car49_logo(uint8_t* rdram, recomp_context* ctx);
void rush2_car49_paint(uint8_t* rdram, recomp_context* ctx);
void rush2_car49_drone(uint8_t* rdram, recomp_context* ctx);
void rush2_car49_dent(uint8_t* rdram, recomp_context* ctx);
void rush2_car49_setup_desc(uint8_t* rdram, recomp_context* ctx);
void rush2_car49_setup_mass(uint8_t* rdram, recomp_context* ctx);
void rush2_car49_engine_before(uint8_t* rdram, recomp_context* ctx);
void rush2_car49_engine_value(uint8_t* rdram, recomp_context* ctx);
void rush2_car49_engine_text(uint8_t* rdram, recomp_context* ctx);
void rush2_car49_option_text(uint8_t* rdram, recomp_context* ctx);
void rush2_car49_bars_begin(uint8_t* rdram, recomp_context* ctx);
void rush2_car49_bars_end(uint8_t* rdram, recomp_context* ctx);
void rush2_car49_select_set(uint8_t* rdram, recomp_context* ctx);
void rush2_car49_select_get(uint8_t* rdram, recomp_context* ctx);
void rush2_car49_asset(uint8_t* rdram, recomp_context* ctx, uint64_t* reg);
void rush2_car49_record(uint8_t* rdram, recomp_context* ctx);
int rush2_car49_dirty(uint8_t* rdram, recomp_context* ctx);
void rush2_track49_circuit(uint8_t* rdram, recomp_context* ctx);
void rush2_track49_race_time(uint8_t* rdram, recomp_context* ctx);
void rush2_track49_circuit_screen(uint8_t* rdram, recomp_context* ctx);
void rush2_track49_overlay_loaded(uint8_t* rdram, recomp_context* ctx);
void rush2_track49_music_command(uint8_t* rdram, recomp_context* ctx);

// SF Rush music (src/track1_audio.cpp).
void rush2_track1_audio_init(uint8_t* rdram, recomp_context* ctx);
void rush2_track1_music(uint8_t* rdram, recomp_context* ctx);
void rush2_track1_song_bank(uint8_t* rdram, recomp_context* ctx);
int rush2_track1_fireworks(uint8_t* rdram, recomp_context* ctx);

// SF Rush breakables (src/track1.cpp).
void rush2_track1_record_model(uint8_t* rdram, recomp_context* ctx);
void rush2_track1_model_name(uint8_t* rdram, recomp_context* ctx);
void rush2_track1_pvs_camera(uint8_t* rdram, recomp_context* ctx);
void rush2_track1_breakable_model(uint8_t* rdram, recomp_context* ctx);
void rush2_track49_movers_car(uint8_t* rdram, recomp_context* ctx);
void rush2_track49_movers_probe_begin(uint8_t* rdram, recomp_context* ctx);
void rush2_track49_movers_probe(uint8_t* rdram, recomp_context* ctx);
void rush2_track49_movers_pads(uint8_t* rdram, recomp_context* ctx);

// Rush 2049 track records (src/track2049_records.cpp).
void rush2_track49_records_stats(uint8_t* rdram, recomp_context* ctx);
void rush2_track49_records_times(uint8_t* rdram, recomp_context* ctx);
void rush2_track49_records_enter(uint8_t* rdram, recomp_context* ctx);
void rush2_track49_records_exit(uint8_t* rdram, recomp_context* ctx);
void rush2_track49_records_seed(uint8_t* rdram, recomp_context* ctx);
void rush2_track49_records_clear(uint8_t* rdram, recomp_context* ctx);

// Bindings and the Controller Setup screen (src/controls_menu.cpp).
void rush2_controls_frame(uint8_t* rdram, recomp_context* ctx);
void rush2_controls_menu_widgets(uint8_t* rdram, recomp_context* ctx);
int rush2_controls_menu_update(uint8_t* rdram, recomp_context* ctx);
int rush2_controls_menu_pause_exit(uint8_t* rdram, recomp_context* ctx);
void rush2_controls_menu_draw(uint8_t* rdram, recomp_context* ctx);
void rush2_wings_menu_labels(uint8_t* rdram, recomp_context* ctx);
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
