#ifndef __RUSH2_HOOKS_H__
#define __RUSH2_HOOKS_H__

// Declarations for runtime functions called from hooks inserted into recompiled code (see us.toml).

#include <stdbool.h>
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

// End of the boot settings load: puts back the ports' present bytes and the no-controllers flag (src/pak.cpp).
void rush2_boot_settings_ports(uint8_t* rdram, recomp_context* ctx);

// Save menu without Controller Paks (src/pak.cpp).
int rush2_pak_menu_state(uint8_t* rdram, recomp_context* ctx);
void rush2_no_pak_manager(uint8_t* rdram, recomp_context* ctx);
void rush2_pak_menu_default_player(uint8_t* rdram, recomp_context* ctx);
void rush2_pak_menu_move_other(uint8_t* rdram, recomp_context* ctx);
void rush2_player_deleted(uint8_t* rdram, recomp_context* ctx);
void rush2_player_deleted_from_list(uint8_t* rdram, recomp_context* ctx);
void rush2_pak_menu_overlay_loaded(uint8_t* rdram, recomp_context* ctx);

// Scales the car's throttle and brake by trigger pressure (src/input.cpp).
void rush2_analog_pedals(uint8_t* rdram, recomp_context* ctx);
void rush2_rumble_impact(uint8_t* rdram, recomp_context* ctx);
void rush2_rumble_wall(uint8_t* rdram, recomp_context* ctx);

// HUD edge anchoring (src/hud.cpp).
void rush2_hud_build_begin(uint8_t* rdram, recomp_context* ctx);
void rush2_hud_build_end(uint8_t* rdram, recomp_context* ctx);
void rush2_hud_finish_begin(uint8_t* rdram, recomp_context* ctx);
void rush2_hud_finish_end(uint8_t* rdram, recomp_context* ctx);
void rush2_hud_widget_created(uint8_t* rdram, recomp_context* ctx);
void rush2_hud_box_begin(uint8_t* rdram, recomp_context* ctx);
void rush2_hud_box_part(uint8_t* rdram, recomp_context* ctx);
void rush2_hud_entry_created(uint8_t* rdram, recomp_context* ctx);
void rush2_hud_highscore_create_begin(uint8_t* rdram, recomp_context* ctx);
void rush2_hud_highscore_create_end(uint8_t* rdram, recomp_context* ctx);
void rush2_hud_draw_begin(uint8_t* rdram, recomp_context* ctx);
void rush2_hud_draw_widget(uint8_t* rdram, recomp_context* ctx);
void rush2_hud_draw_end(uint8_t* rdram, recomp_context* ctx);
void rush2_hud_print(uint8_t* rdram, recomp_context* ctx);
void rush2_hud_2d_list_start(uint8_t* rdram, recomp_context* ctx);
void rush2_hud_2d_list_end(uint8_t* rdram, recomp_context* ctx);
void rush2_hud_stunt_text(uint8_t* rdram, recomp_context* ctx, int inside);
void rush2_hud_laps_begin(uint8_t* rdram, recomp_context* ctx);
void rush2_hud_laps_end(uint8_t* rdram, recomp_context* ctx);

// Frame interpolation matrix group tagging (src/interpolation.cpp).
void rush2_interp_view_begin(uint8_t* rdram, recomp_context* ctx);
void rush2_car49_flames_view_begin(uint8_t* rdram, recomp_context* ctx);
void rush2_car49_flames_draw(uint8_t* rdram, recomp_context* ctx);
void rush2_interp_projection(uint8_t* rdram, recomp_context* ctx);
void rush2_interp_root_modelview(uint8_t* rdram, recomp_context* ctx);
void rush2_interp_poly(uint8_t* rdram, recomp_context* ctx);
void rush2_interp_poly_vtx(uint8_t* rdram, recomp_context* ctx);
void rush2_interp_poly_end(uint8_t* rdram, recomp_context* ctx);
void rush2_sky_depth(uint8_t* rdram, recomp_context* ctx);
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
void rush2_title_background_tiles(uint8_t* rdram, recomp_context* ctx);
void rush2_frame_clear_end(uint8_t* rdram, recomp_context* ctx);
void rush2_model_draw(uint8_t* rdram, recomp_context* ctx);

// Three and four player races (src/players4.cpp).
void rush2_players4_init(uint8_t* rdram, recomp_context* ctx);
void rush2_players4_frame_buffers(uint8_t* rdram, recomp_context* ctx);
void rush2_players4_race_start(uint8_t* rdram, recomp_context* ctx);
void rush2_players4_stunt_boxes(uint8_t* rdram, recomp_context* ctx);
int rush2_players4_cars_chosen(uint8_t* rdram, recomp_context* ctx);
int rush2_players4_cars_back(uint8_t* rdram, recomp_context* ctx);
int rush2_players4_all_finished(uint8_t* rdram, recomp_context* ctx);
void rush2_players4_car_label(uint8_t* rdram, recomp_context* ctx);
void rush2_players4_menu(uint8_t* rdram, recomp_context* ctx);
void rush2_players4_state(uint8_t* rdram, recomp_context* ctx);
void rush2_players4_title_port(uint8_t* rdram, recomp_context* ctx);
void rush2_title_screen_setup(uint8_t* rdram, recomp_context* ctx);
int rush2_players4_skip_finish_box(uint8_t* rdram, recomp_context* ctx);
int rush2_players4_finish_rebuild(uint8_t* rdram, recomp_context* ctx);
void rush2_players4_race_views(uint8_t* rdram, recomp_context* ctx);
void rush2_players4_frame_views(uint8_t* rdram, recomp_context* ctx);
void rush2_players4_list_offset(uint8_t* rdram, recomp_context* ctx);
void rush2_players4_poly_mask(uint8_t* rdram, recomp_context* ctx);
void rush2_players4_sky_view(uint8_t* rdram, recomp_context* ctx);
void rush2_players4_sky_fade(uint8_t* rdram, recomp_context* ctx);

// Split screen layout (src/splitscreen.cpp).
void rush2_split_views_begin(uint8_t* rdram, recomp_context* ctx);
void rush2_split_views_end(uint8_t* rdram, recomp_context* ctx);
void rush2_split_divider(uint8_t* rdram, recomp_context* ctx);

// Level of detail override (src/lod.cpp).
void rush2_lod_select(uint8_t* rdram, recomp_context* ctx);

// Draw distance (src/draw_distance.cpp).
void rush2_draw_distance_projection(uint8_t* rdram, recomp_context* ctx);
void rush2_draw_distance_cull(uint8_t* rdram, recomp_context* ctx);
int rush2_draw_distance_matrix_rows(uint8_t* rdram, recomp_context* ctx);
int rush2_draw_distance_matrix_cols(uint8_t* rdram, recomp_context* ctx);

// Applies the Cheats tab's settings once per frame (src/cheats.cpp).
void rush2_cheats_frame(uint8_t* rdram, recomp_context* ctx);
void rush2_cheats_car_list(uint8_t* rdram, recomp_context* ctx);
void rush2_cheats_kill_floor(uint8_t* rdram, recomp_context* ctx);

// High-resolution font tile clamping around the 2D image loader (src/fonts.cpp).
void rush2_font_load_begin(uint8_t* rdram, recomp_context* ctx);
void rush2_font_load_end(uint8_t* rdram, recomp_context* ctx);

// Replacement asset files and the relocated game heap (src/assets.cpp).
int rush2_asset_decompress(uint8_t* rdram, recomp_context* ctx);
void rush2_heap_init(uint8_t* rdram, recomp_context* ctx);

// Rush 2049 tracks raced in a borrowed track slot (src/rush2049/track2049.cpp).
void rush2_track49_load(uint8_t* rdram, recomp_context* ctx);
void rush2_trackmod_drag_dirt(uint8_t* rdram, recomp_context* ctx);
void rush2_trackmod_drag_grass(uint8_t* rdram, recomp_context* ctx);
void rush2_trackmod_torque_dirt(uint8_t* rdram, recomp_context* ctx);
void rush2_trackmod_torque_grass(uint8_t* rdram, recomp_context* ctx);
void rush2_trackmod_wheel_material(uint8_t* rdram, recomp_context* ctx);
void rush2_trackmod_wheel_fallback(uint8_t* rdram, recomp_context* ctx);
// Another game's wheel surface effects on its converted tracks (src/surface_fx.cpp).
void rush2_surface_wheel_material(uint8_t* rdram, recomp_context* ctx);
void rush2_surface_wheel_fallback(uint8_t* rdram, recomp_context* ctx);
void rush2_surface_particle(uint8_t* rdram, recomp_context* ctx);
void rush2_surface_mark_rule(uint8_t* rdram, recomp_context* ctx);
void rush2_surface_mark_start(uint8_t* rdram, recomp_context* ctx);
void rush2_surface_mark_extend(uint8_t* rdram, recomp_context* ctx);
void rush2_surface_mark_restart(uint8_t* rdram, recomp_context* ctx);
void rush2_surface_mark_fade(uint8_t* rdram, recomp_context* ctx);
void rush2_surface_mark_done(uint8_t* rdram, recomp_context* ctx);
void rush2_surface_race_cars(uint8_t* rdram, recomp_context* ctx);
void rush2_surface_car_draw(uint8_t* rdram, recomp_context* ctx);
void rush2_track49_pvs(uint8_t* rdram, recomp_context* ctx);
void rush2_track49_sky(uint8_t* rdram, recomp_context* ctx);
void rush2_track49_sky_players(uint8_t* rdram, recomp_context* ctx);
void rush2_track49_sky_height(uint8_t* rdram, recomp_context* ctx);
void rush2_track49_select_init(uint8_t* rdram, recomp_context* ctx);
void rush2_track49_select_count(uint8_t* rdram, recomp_context* ctx);
void rush2_track49_carousel_order(uint8_t* rdram, recomp_context* ctx);
void rush2_track49_select_step(uint8_t* rdram, recomp_context* ctx);
void rush2_track49_select_model(uint8_t* rdram, recomp_context* ctx);
void rush2_track49_select_pose(uint8_t* rdram, recomp_context* ctx);
int rush2_track49_select_available(uint8_t* rdram, recomp_context* ctx);
void rush2_track49_select_wrap(uint8_t* rdram, recomp_context* ctx);
void rush2_track49_select_save_p1(uint8_t* rdram, recomp_context* ctx);
void rush2_track49_select_save_p2(uint8_t* rdram, recomp_context* ctx);
int rush2_track49_keys(uint8_t* rdram, recomp_context* ctx);
void rush2_track49_race_start(uint8_t* rdram, recomp_context* ctx);
void rush2_track49_movers_tick(uint8_t* rdram, recomp_context* ctx);
void rush2_car49_list(uint8_t* rdram, recomp_context* ctx);
void rush2_car49_logo(uint8_t* rdram, recomp_context* ctx);
void rush2_car49_paint(uint8_t* rdram, recomp_context* ctx);
void rush2_car49_drone(uint8_t* rdram, recomp_context* ctx);
void rush2_car49_dent(uint8_t* rdram, recomp_context* ctx);
bool rush2_calspeed_brake_lights(uint8_t* rdram, recomp_context* ctx);
void rush2_livery_mirror_begin(uint8_t* rdram, recomp_context* ctx);
void rush2_livery_mirror_end(uint8_t* rdram, recomp_context* ctx);
void rush2_car49_setup_desc(uint8_t* rdram, recomp_context* ctx);
void rush2_car49_setup_mass(uint8_t* rdram, recomp_context* ctx);
void rush2_car49_engine_before(uint8_t* rdram, recomp_context* ctx);
void rush2_car49_engine_value(uint8_t* rdram, recomp_context* ctx);
void rush2_car49_engine_text(uint8_t* rdram, recomp_context* ctx);
void rush2_car49_tires_text(uint8_t* rdram, recomp_context* ctx);
void rush2_car49_tires_before(uint8_t* rdram, recomp_context* ctx);
void rush2_car49_tires_value(uint8_t* rdram, recomp_context* ctx);
void rush2_car49_steer_add(uint8_t* rdram, recomp_context* ctx);
void rush2_car49_yaw_add(uint8_t* rdram, recomp_context* ctx);
int rush2_car49_offroad(uint8_t* rdram, recomp_context* ctx);

void rush2_car49_option_text(uint8_t* rdram, recomp_context* ctx);
void rush2_car49_bars_begin(uint8_t* rdram, recomp_context* ctx);
void rush2_car49_bars_end(uint8_t* rdram, recomp_context* ctx);
void rush2_car49_select_set(uint8_t* rdram, recomp_context* ctx);
void rush2_car49_select_get(uint8_t* rdram, recomp_context* ctx);
void rush2_car49_select_cursor(uint8_t* rdram, recomp_context* ctx);
void rush2_car49_select_first_frame(uint8_t* rdram, recomp_context* ctx);
void rush2_car49_asset(uint8_t* rdram, recomp_context* ctx, uint64_t* reg);
void rush2_car49_asset_size(uint8_t* rdram, recomp_context* ctx, uint64_t* reg);

// The car select's engine rev (src/engine_preview.cpp).
void rush2_engine_preview_car_select(uint8_t* rdram, recomp_context* ctx);
void rush2_engine_preview_start(uint8_t* rdram, recomp_context* ctx);
void rush2_engine_preview_frame(uint8_t* rdram, recomp_context* ctx);
void rush2_engine_preview_stop(uint8_t* rdram, recomp_context* ctx);
// Rush 2049 engine sounds for the 2049 cars (src/rush2049/engine2049.cpp).
int rush2_engine49_start(uint8_t* rdram, recomp_context* ctx);
void rush2_engine49_stop(uint8_t* rdram, recomp_context* ctx);
void rush2_engine49_tick(uint8_t* rdram, recomp_context* ctx);
void rush2_car49_record(uint8_t* rdram, recomp_context* ctx);
int rush2_car49_dirty(uint8_t* rdram, recomp_context* ctx);
// The SF Rush car stripe (src/rush1/car1_stripes.cpp).
int rush2_car1_stripe_get(uint8_t* rdram, recomp_context* ctx);
void rush2_car1_stripe_set(uint8_t* rdram, recomp_context* ctx);
void rush2_car1_stripe_before(uint8_t* rdram, recomp_context* ctx);
void rush2_car1_stripe_value(uint8_t* rdram, recomp_context* ctx);
void rush2_car1_stripe_text_row(uint8_t* rdram, recomp_context* ctx);
void rush2_car1_stripe_text(uint8_t* rdram, recomp_context* ctx);
void rush2_car1_paint_style(uint8_t* rdram, recomp_context* ctx);
void rush2_car1_paint_stamp(uint8_t* rdram, recomp_context* ctx);
void rush2_track49_circuit(uint8_t* rdram, recomp_context* ctx);
void rush2_race_lane_speeds(uint8_t* rdram, recomp_context* ctx);
void rush2_speedometer(uint8_t* rdram, recomp_context* ctx);
void rush2_track49_race_time(uint8_t* rdram, recomp_context* ctx);
bool rush2_mod_checkpoint_crossing(uint8_t* rdram, recomp_context* ctx, bool lane);
void rush2_track49_circuit_screen(uint8_t* rdram, recomp_context* ctx);

// Circuit length and courses (src/circuit.cpp).
void rush2_circuit_started(uint8_t* rdram, recomp_context* ctx);
void rush2_circuit_race_index(uint8_t* rdram, recomp_context* ctx);
void rush2_circuit_complete_t7(uint8_t* rdram, recomp_context* ctx);
void rush2_circuit_complete_t9(uint8_t* rdram, recomp_context* ctx);
void rush2_circuit_complete_v0(uint8_t* rdram, recomp_context* ctx);
void rush2_circuit_last_race(uint8_t* rdram, recomp_context* ctx);
void rush2_circuit_races_left(uint8_t* rdram, recomp_context* ctx);
void rush2_circuit_more_input(uint8_t* rdram, recomp_context* ctx);
void rush2_circuit_more_grey(uint8_t* rdram, recomp_context* ctx);
void rush2_circuit_more_step(uint8_t* rdram, recomp_context* ctx);
void rush2_circuit_more_stepped(uint8_t* rdram, recomp_context* ctx);
int rush2_circuit_fixed_difficulty(uint8_t* rdram, recomp_context* ctx);
// Accurate Circuit Review (src/circuit.cpp).
void rush2_circuit_review_snapshot(uint8_t* rdram, recomp_context* ctx);
void rush2_circuit_review_title(uint8_t* rdram, recomp_context* ctx);
void rush2_circuit_review_title_logo(uint8_t* rdram, recomp_context* ctx);
void rush2_circuit_review_first(uint8_t* rdram, recomp_context* ctx);
void rush2_circuit_review_preload(uint8_t* rdram, recomp_context* ctx);
void rush2_circuit_review_preload_logo(uint8_t* rdram, recomp_context* ctx);
void rush2_circuit_review_next(uint8_t* rdram, recomp_context* ctx);
void rush2_circuit_review_demo(uint8_t* rdram, recomp_context* ctx);
void rush2_circuit_review_car(uint8_t* rdram, recomp_context* ctx);
void rush2_circuit_race_car(uint8_t* rdram, recomp_context* ctx);
void rush2_track49_overlay_loaded(uint8_t* rdram, recomp_context* ctx);
void rush2_track49_music_command(uint8_t* rdram, recomp_context* ctx);
void rush2_track49_stunt_mode(uint8_t* rdram, recomp_context* ctx);
void rush2_track49_stunt_settings_t8(uint8_t* rdram, recomp_context* ctx);
void rush2_track49_stunt_settings_t6(uint8_t* rdram, recomp_context* ctx);
void rush2_track49_stunt_select(uint8_t* rdram, recomp_context* ctx);
void rush2_track49_stunt_options(uint8_t* rdram, recomp_context* ctx);
void rush2_track49_stunt_option_t9(uint8_t* rdram, recomp_context* ctx);
void rush2_track49_stunt_option_t8(uint8_t* rdram, recomp_context* ctx);
void rush2_battle_hide_stunt_panel(uint8_t* rdram, recomp_context* ctx);
void rush2_battle_time_left_cheat(uint8_t* rdram, recomp_context* ctx);
// func_8007C27C entry (a view's visibility, before the view is drawn): the battle HUD's 3D models of that view.
void rush2_battle_view(uint8_t* rdram, recomp_context* ctx);
void rush2_battle_ram(uint8_t* rdram, recomp_context* ctx);
void rush2_battle_race_steer(uint8_t* rdram, recomp_context* ctx);
void rush2_arrows_view(uint8_t* rdram, recomp_context* ctx);
void rush2_battle_render_view(uint8_t* rdram, recomp_context* ctx);
void rush2_track49_deaths_value_t4(uint8_t* rdram, recomp_context* ctx);
void rush2_track49_deaths_value_t0(uint8_t* rdram, recomp_context* ctx);
void rush2_track49_drones_value(uint8_t* rdram, recomp_context* ctx);
void rush2_track49_battle_rule_label(uint8_t* rdram, recomp_context* ctx);
void rush2_track49_battle_rule_arrows(uint8_t* rdram, recomp_context* ctx);
int rush2_track49_battle_rule_value(uint8_t* rdram, recomp_context* ctx);
int rush2_track49_battle_rule_step(uint8_t* rdram, recomp_context* ctx);
int rush2_track49_battle_label(uint8_t* rdram, recomp_context* ctx);
int rush2_track49_battle_arrow_label(uint8_t* rdram, recomp_context* ctx);
int rush2_track49_battle_value(uint8_t* rdram, recomp_context* ctx);
int rush2_track49_battle_step(uint8_t* rdram, recomp_context* ctx);
void rush2_track49_option_scroll(uint8_t* rdram, recomp_context* ctx);
void rush2_track49_option_box(uint8_t* rdram, recomp_context* ctx);
void rush2_track49_option_text_rows(uint8_t* rdram, recomp_context* ctx);
void rush2_track49_obstacle_settings(uint8_t* rdram, recomp_context* ctx);
void rush2_track49_obstacle_clock(uint8_t* rdram, recomp_context* ctx);
void rush2_track49_stuck_timer(uint8_t* rdram, recomp_context* ctx);
void rush2_track49_respawn_delay(uint8_t* rdram, recomp_context* ctx);
void rush2_ghost_leaf_filter(uint8_t* rdram, recomp_context* ctx, uint32_t list, uint32_t car);
void rush2_track49_gravity(uint8_t* rdram, float* gravity);
int rush2_track49_respawn_zone(uint8_t* rdram, recomp_context* ctx);
void rush2_track49_respawn_zone_advance(uint8_t* rdram, recomp_context* ctx);
void rush2_track49_respawn_zone_place(uint8_t* rdram, recomp_context* ctx);
void rush2_track49_battle_start(uint8_t* rdram, recomp_context* ctx);
int rush2_track49_battle_arena(uint8_t* rdram);
void rush2_track49_battle_rearm(uint8_t* rdram, recomp_context* ctx);
void rush2_track49_map_s4(uint8_t* rdram, recomp_context* ctx);
void rush2_track49_map_a1(uint8_t* rdram, recomp_context* ctx);
void rush2_track49_map_t0(uint8_t* rdram, recomp_context* ctx);
void rush2_track49_map_t2(uint8_t* rdram, recomp_context* ctx);
void rush2_mode_menu_choose(uint8_t* rdram, recomp_context* ctx);
void rush2_mode_menu_last_row(uint8_t* rdram, recomp_context* ctx);
void rush2_mode_menu_below(uint8_t* rdram, recomp_context* ctx);
void rush2_mode_menu_label_count(uint8_t* rdram, recomp_context* ctx);
void rush2_mode_menu_label_t3(uint8_t* rdram, recomp_context* ctx);
void rush2_mode_menu_label_t1(uint8_t* rdram, recomp_context* ctx);
void rush2_mode_menu_box(uint8_t* rdram, recomp_context* ctx);
void rush2_mode_menu_bar(uint8_t* rdram, recomp_context* ctx);

// Race music and song previews (src/music.cpp).
void rush2_music_race(uint8_t* rdram, recomp_context* ctx);
void rush2_music_race_sequence(uint8_t* rdram, recomp_context* ctx);
void rush2_music_preview(uint8_t* rdram, recomp_context* ctx);

// SF Rush music (src/rush1/track1_audio.cpp).
void rush2_track1_audio_init(uint8_t* rdram, recomp_context* ctx);
void rush2_track1_song_bank(uint8_t* rdram, recomp_context* ctx);
int rush2_track1_fireworks(uint8_t* rdram, recomp_context* ctx);

// SF Rush track 6 buses (src/rush1/track1_buses.cpp).
int rush2_track1_buses_init(uint8_t* rdram, recomp_context* ctx);
int rush2_track1_buses_update(uint8_t* rdram, recomp_context* ctx);
void rush2_track1_buses_collision(uint8_t* rdram, recomp_context* ctx);
void rush2_track1_buses_drive(uint8_t* rdram, recomp_context* ctx);
void rush2_track1_gate(uint8_t* rdram, recomp_context* ctx);

// SF Rush breakables (src/rush1/track1.cpp).
void rush2_track1_record_model(uint8_t* rdram, recomp_context* ctx);
void rush2_track1_model_name(uint8_t* rdram, recomp_context* ctx);
void rush2_track1_pvs_camera(uint8_t* rdram, recomp_context* ctx);
void rush2_track1_breakable_model(uint8_t* rdram, recomp_context* ctx);
int rush2_track1_breakable_hit(uint8_t* rdram, recomp_context* ctx);
void rush2_track1_wrong_way(uint8_t* rdram, recomp_context* ctx);
void rush2_track49_movers_car(uint8_t* rdram, recomp_context* ctx);
void rush2_track49_movers_probe_begin(uint8_t* rdram, recomp_context* ctx);
void rush2_track49_movers_probe(uint8_t* rdram, recomp_context* ctx);
void rush2_track49_movers_pads(uint8_t* rdram, recomp_context* ctx);
// Rush 2049's collision lookups on its converted courses (src/rush2049/track2049_collision.cpp).
void rush2_track49_collision_load(uint8_t* rdram, recomp_context* ctx);
int rush2_collision_leaf_read_wide(uint8_t* rdram, recomp_context* ctx);
void rush2_collision_leaf_list(recomp_context* ctx, gpr* reg, uint32_t stack_offset, int query);
uint32_t rush2_collision_leaf_list_at(int query);
void rush2_track49_collision_split(uint8_t* rdram, recomp_context* ctx);
void rush2_track49_collision_cell(uint8_t* rdram, recomp_context* ctx);
void rush2_track49_collision_cell_basis(uint8_t* rdram, recomp_context* ctx);
void rush2_track49_collision_leaf_ground(uint8_t* rdram, recomp_context* ctx);
void rush2_track49_collision_leaf_wheel(uint8_t* rdram, recomp_context* ctx);
void rush2_track49_collision_leaf_wall(uint8_t* rdram, recomp_context* ctx);
void rush2_track49_collision_leaf_basis(uint8_t* rdram, recomp_context* ctx);
void rush2_track49_collision_selftest(uint8_t* rdram, recomp_context* ctx);

// Rush 2049 track records (src/rush2049/track2049_records.cpp).
void rush2_track49_records_stats(uint8_t* rdram, recomp_context* ctx);
void rush2_track49_records_times(uint8_t* rdram, recomp_context* ctx);
void rush2_track49_records_enter(uint8_t* rdram, recomp_context* ctx);
void rush2_track49_records_exit(uint8_t* rdram, recomp_context* ctx);
void rush2_track49_records_seed(uint8_t* rdram, recomp_context* ctx);
void rush2_track49_records_clear(uint8_t* rdram, recomp_context* ctx);
void rush2_records_screen_open(uint8_t* rdram, recomp_context* ctx);
void rush2_records_screen_close(uint8_t* rdram, recomp_context* ctx);
void rush2_records_screen_step(uint8_t* rdram, recomp_context* ctx);
void rush2_records_screen_render_enter(uint8_t* rdram, recomp_context* ctx);
void rush2_records_screen_render_exit(uint8_t* rdram, recomp_context* ctx);
void rush2_records_screen_times(uint8_t* rdram, recomp_context* ctx);
void rush2_records_screen_stats(uint8_t* rdram, recomp_context* ctx);
int rush2_records_screen_icon(uint8_t* rdram, recomp_context* ctx);

// The unlock system's shop (src/unlocks_shop.cpp).
int rush2_unlocks_track_select(uint8_t* rdram, recomp_context* ctx);
void rush2_unlocks_list_text_begin(uint8_t* rdram, recomp_context* ctx);
void rush2_unlocks_list_text_end(uint8_t* rdram, recomp_context* ctx);
int rush2_unlocks_list_choose(uint8_t* rdram, recomp_context* ctx);
int rush2_unlocks_carousel_car(uint8_t* rdram, recomp_context* ctx);
void rush2_unlocks_list_cursor(uint8_t* rdram, recomp_context* ctx);
int rush2_unlocks_shop_frame(uint8_t* rdram, recomp_context* ctx);
void rush2_unlocks_widgets(uint8_t* rdram, recomp_context* ctx);
int rush2_unlocks_text(uint8_t* rdram, recomp_context* ctx);

// SF Rush keys and Rush 2049 coins on the added tracks (src/collectibles.cpp).
void rush2_collect_race_reset(uint8_t* rdram, recomp_context* ctx);
void rush2_collect_frame(uint8_t* rdram, recomp_context* ctx);
void rush2_collect_number(uint8_t* rdram, recomp_context* ctx);
int rush2_collect_taken(uint8_t* rdram, recomp_context* ctx);
int rush2_collect_take(uint8_t* rdram, recomp_context* ctx);
int rush2_collect_hit(uint8_t* rdram, recomp_context* ctx);
void rush2_collect_record_model(uint8_t* rdram, recomp_context* ctx);
void rush2_collect_breakable_model(uint8_t* rdram, recomp_context* ctx);

// Bindings and the Controller Setup screen (src/controls_menu.cpp).
void rush2_controls_frame(uint8_t* rdram, recomp_context* ctx);
void rush2_controls_menu_widgets(uint8_t* rdram, recomp_context* ctx);
int rush2_controls_menu_update(uint8_t* rdram, recomp_context* ctx);
int rush2_controls_menu_pause_exit(uint8_t* rdram, recomp_context* ctx);
void rush2_controls_menu_draw(uint8_t* rdram, recomp_context* ctx);
void rush2_controls_menu_text_players(uint8_t* rdram, recomp_context* ctx);
void rush2_controls_menu_title(uint8_t* rdram, recomp_context* ctx);
void rush2_players4_paused(uint8_t* rdram, recomp_context* ctx);
void rush2_wings_menu_labels(uint8_t* rdram, recomp_context* ctx);
void rush2_controls_menu_box(uint8_t* rdram, recomp_context* ctx);
void rush2_controls_menu_title_y(uint8_t* rdram, recomp_context* ctx);
void rush2_controls_menu_first_row(uint8_t* rdram, recomp_context* ctx);
void rush2_wings_menu_label_row(uint8_t* rdram, recomp_context* ctx);

// California Speed tracks: cars under the course and the road hold (src/calspeed/calspeed_rom.cpp).
void rush2_calspeed_car_frame(uint8_t* rdram, recomp_context* ctx);
void rush2_calspeed_road_hold(uint8_t* rdram, recomp_context* ctx);
void rush2_calspeed_wheel_damper(uint8_t* rdram, recomp_context* ctx);
void rush2_calspeed_tire_load(uint8_t* rdram, recomp_context* ctx);
void rush2_calspeed_fall_floor(uint8_t* rdram, recomp_context* ctx);
int rush2_calspeed_finished_car(uint8_t* rdram, recomp_context* ctx);
void rush2_calspeed_demo_start(uint8_t* rdram, recomp_context* ctx);
void rush2_calspeed_respawn_advance(uint8_t* rdram, recomp_context* ctx);
void rush2_calspeed_respawn_place(uint8_t* rdram, recomp_context* ctx);
void rush2_calspeed_wheel_support(uint8_t* rdram, recomp_context* ctx);
// California Speed's walls and missed wheels (src/calspeed/calspeed_walls.cpp) and physics (calspeed_physics.cpp),
// with the CS Physics option.
int rush2_calspeed_wall_collision(uint8_t* rdram, recomp_context* ctx);
void rush2_calspeed_ground_contact_enter(uint8_t* rdram, recomp_context* ctx);
void rush2_calspeed_ground_contact_leave(uint8_t* rdram, recomp_context* ctx);
void rush2_calspeed_probe_miss(uint8_t* rdram, recomp_context* ctx);
void rush2_csphys_init_begin(uint8_t* rdram, recomp_context* ctx);
void rush2_csphys_init_end(uint8_t* rdram, recomp_context* ctx);
void rush2_csphys_probe(uint8_t* rdram, recomp_context* ctx);
void rush2_csphys_wheel_forces(uint8_t* rdram, recomp_context* ctx);
void rush2_csphys_torque(uint8_t* rdram, recomp_context* ctx);
void rush2_csphys_orient_begin(uint8_t* rdram, recomp_context* ctx);
void rush2_csphys_orient_end(uint8_t* rdram, recomp_context* ctx);
int rush2_csphys_velocities(uint8_t* rdram, recomp_context* ctx);
int rush2_csphys_shift(uint8_t* rdram, recomp_context* ctx);
void rush2_csphys_wreck_log(uint8_t* rdram, recomp_context* ctx);
// Directional rumble (src/rumble.cpp): a car's contact with a wall of the given world normal.
void rush2_rumble_wall_contact(uint8_t* rdram, uint32_t car, const float* normal);

// Smooth-shaded collision on California Speed and custom tracks (src/smooth_collision.cpp).
void rush2_smooth_collision_surface(uint8_t* rdram, recomp_context* ctx);
void rush2_smooth_collision_leaf(uint8_t* rdram, recomp_context* ctx, int count);
bool rush2_smooth_collision_gap(uint8_t* rdram, recomp_context* ctx);
void rush2_smooth_debug_contact(uint8_t* rdram, recomp_context* ctx);

// Rush 2049 wings physics and drawing (src/rush2049/wings_state.cpp, src/rush2049/wings_render.cpp).
void rush2_wings_torque(uint8_t* rdram, recomp_context* ctx);
void rush2_wings_save_position(uint8_t* rdram, recomp_context* ctx);
void rush2_wings_gravity(uint8_t* rdram, recomp_context* ctx);
void rush2_wings_drag(uint8_t* rdram, recomp_context* ctx);
void rush2_wings_car_init(uint8_t* rdram, recomp_context* ctx);
void rush2_wings_model_draw(uint8_t* rdram, recomp_context* ctx);

// Ghost races (src/ghost.cpp).
void rush2_ghost_settings(uint8_t* rdram, recomp_context* ctx);
void rush2_ghost_race_setup(uint8_t* rdram, recomp_context* ctx);
// Rush 2049's odometer (src/rush2049/odometer.cpp): the race setup, also on a restart, starts the distances over.
void rush2_odometer_race_setup(uint8_t* rdram, recomp_context* ctx);
int rush2_stunt2049_update(uint8_t* rdram, recomp_context* ctx);
void rush2_stunt2049_body_hit(uint8_t* rdram, recomp_context* ctx);
void rush2_stunt2049_wreck(uint8_t* rdram, recomp_context* ctx, int car);
void rush2_stunt2049_hud(uint8_t* rdram, recomp_context* ctx, int step);
void rush2_stunt2049_race_setup(uint8_t* rdram, recomp_context* ctx);
void rush2_stunt2049_box_visible(uint8_t* rdram, recomp_context* ctx);
void rush2_stunt2049_panel_text(uint8_t* rdram, recomp_context* ctx);
void rush2_ghost_drone_type(uint8_t* rdram, recomp_context* ctx);
void rush2_ghost_drone_colors(uint8_t* rdram, recomp_context* ctx);
int rush2_ghost_car_tick(uint8_t* rdram, recomp_context* ctx);
int rush2_ghost_drive(uint8_t* rdram, recomp_context* ctx);
void rush2_ghost_tick_end(uint8_t* rdram, recomp_context* ctx);
// Battle arenas' computer opponents (src/rush2049/battle_ai.cpp).
int rush2_battle_ai_drive(uint8_t* rdram, recomp_context* ctx);
int rush2_battle_ai_car_tick(uint8_t* rdram, recomp_context* ctx);
void rush2_battle_ai_tick_end(uint8_t* rdram, recomp_context* ctx);
// Race recorder for tools/trackview (src/race_record.cpp, RUSH2_RACE_RECORD).
void rush2_race_record_tick(uint8_t* rdram, recomp_context* ctx);
void rush2_ghost_collide_self(uint8_t* rdram, recomp_context* ctx);
void rush2_ghost_collide_other(uint8_t* rdram, recomp_context* ctx);
void rush2_ghost_standings(uint8_t* rdram, recomp_context* ctx);
void rush2_ghost_rubber_band(uint8_t* rdram, recomp_context* ctx);
void rush2_ghost_map_dot(uint8_t* rdram, recomp_context* ctx);
void rush2_ghost_radar_dot(uint8_t* rdram, recomp_context* ctx);
void rush2_ghost_fog_color(uint8_t* rdram, recomp_context* ctx);
void rush2_ghost_car_select_text(uint8_t* rdram, recomp_context* ctx);
int rush2_track49_filter_frame(uint8_t* rdram, recomp_context* ctx);
void rush2_track49_filter_text(uint8_t* rdram, recomp_context* ctx);
int rush2_filter_car_frame(uint8_t* rdram, recomp_context* ctx);
void rush2_filter_car_text(uint8_t* rdram, recomp_context* ctx);
void rush2_filter_car_cursor(uint8_t* rdram, recomp_context* ctx);
void rush2_wings_car_rows(uint8_t* rdram, recomp_context* ctx);
void rush2_wings_car_step(uint8_t* rdram, recomp_context* ctx);
void rush2_wings_car_label_t7(uint8_t* rdram, recomp_context* ctx);
void rush2_wings_car_label_t4(uint8_t* rdram, recomp_context* ctx);
void rush2_wings_car_arrow_t9(uint8_t* rdram, recomp_context* ctx);
void rush2_wings_car_arrow_t2(uint8_t* rdram, recomp_context* ctx);
void rush2_wings_car_value(uint8_t* rdram, recomp_context* ctx);
void rush2_car_rows_built(uint8_t* rdram, recomp_context* ctx);
void rush2_fix_dew_paint_rows(uint8_t* rdram, recomp_context* ctx);
void rush2_fix_engines_after_timeout(uint8_t* rdram, recomp_context* ctx);
void rush2_fix_white_accent(uint8_t* rdram, recomp_context* ctx);
void rush2_fix_formula_rear_wheels(uint8_t* rdram, recomp_context* ctx);
void rush2_breakable_groups_s3(uint8_t* rdram, recomp_context* ctx);
void rush2_breakable_groups_s6(uint8_t* rdram, recomp_context* ctx);
void rush2_breakable_groups_t8(uint8_t* rdram, recomp_context* ctx);
void rush2_breakable_groups_a2(uint8_t* rdram, recomp_context* ctx);
void rush2_fix_finish_place(uint8_t* rdram, recomp_context* ctx);
void rush2_fix_drone_glide_node(uint8_t* rdram, recomp_context* ctx);
void rush2_fix_drone_glide_hide(uint8_t* rdram, recomp_context* ctx);
void rush2_stunt_respawn_gear(uint8_t* rdram, recomp_context* ctx);
void rush2_stunt_respawn_gear_reset(uint8_t* rdram, recomp_context* ctx);
void rush2_music_pause_start(uint8_t* rdram, recomp_context* ctx);
void rush2_music_continue(uint8_t* rdram, recomp_context* ctx);
void rush2_music_pause_frame(uint8_t* rdram, recomp_context* ctx);
void rush2_music_pause_release(uint8_t* rdram, recomp_context* ctx);
void rush2_pause_widgets(uint8_t* rdram, recomp_context* ctx);
void rush2_pause_menu_input(uint8_t* rdram, recomp_context* ctx);
void rush2_drone_catch_up(uint8_t* rdram, recomp_context* ctx);
void rush2_car_rows_frame(uint8_t* rdram, recomp_context* ctx);
void rush2_car_rows_text(uint8_t* rdram, recomp_context* ctx);
void rush2_car_rows_widget(uint8_t* rdram, recomp_context* ctx, int shift, int swatch);
void rush2_ghost_drones_step(uint8_t* rdram, recomp_context* ctx);
void rush2_ghost_drones_reset(uint8_t* rdram, recomp_context* ctx);
void rush2_ghost_drones_value(uint8_t* rdram, recomp_context* ctx);
void rush2_ghost_drones_digit(uint8_t* rdram, recomp_context* ctx);
int32_t rush2_ghost_hud_cars(int32_t cars);
void rush2_ghost_draft(uint8_t* rdram, recomp_context* ctx);
int rush2_ghost_breakable_hit(uint8_t* rdram, recomp_context* ctx);

// AI Opponents (src/npc_cars.cpp).
void rush2_npc_cars_apply(uint8_t* rdram, recomp_context* ctx);
void rush2_npc_cars_building(uint8_t* rdram, recomp_context* ctx);
void rush2_npc_cars_rim_nodes(uint8_t* rdram, recomp_context* ctx);
void rush2_npc_cars_rim_frame(uint8_t* rdram, recomp_context* ctx);
void rush2_npc_cars_rim_state(uint8_t* rdram, recomp_context* ctx);
void rush2_npc_cars_rim_init(uint8_t* rdram, recomp_context* ctx);

// Paint colors with the Rush 2049 shades (src/paint_colors.cpp).
void rush2_paint_rgba_table(uint8_t* rdram, recomp_context* ctx, uint64_t* reg);
void rush2_paint_5551_table(uint8_t* rdram, recomp_context* ctx, uint64_t* reg);
void rush2_paint_convert(uint8_t* rdram, recomp_context* ctx);
void rush2_paint_get(uint8_t* rdram, recomp_context* ctx, uint32_t at, int field, uint64_t* value);
void rush2_paint_set(uint8_t* rdram, recomp_context* ctx, int field);
void rush2_paint_step_before(uint8_t* rdram, recomp_context* ctx, int value);
void rush2_paint_step_value(uint8_t* rdram, recomp_context* ctx, int field);
// Online play (src/online/online_game.cpp).
void rush2_online_frame(uint8_t* rdram, recomp_context* ctx);
// RUSH2_TEST_RACE: a race straight from the title screen (src/test_race.cpp).
void rush2_test_race_frame(uint8_t* rdram, recomp_context* ctx);
void rush2_online_frame_time(uint8_t* rdram, recomp_context* ctx);
int rush2_online_skip_physics(uint8_t* rdram, recomp_context* ctx);
void rush2_online_player_input(uint8_t* rdram, recomp_context* ctx);
int rush2_online_pause_start_only(uint8_t* rdram, recomp_context* ctx);
void rush2_online_race_views(uint8_t* rdram, recomp_context* ctx);
void rush2_online_record_presses(uint8_t* rdram, recomp_context* ctx);
void rush2_online_rumble_port(uint8_t* rdram, recomp_context* ctx);
void rush2_online_present_begin(uint8_t* rdram, recomp_context* ctx);
void rush2_online_present_end(uint8_t* rdram, recomp_context* ctx);
int rush2_online_sim_view(uint8_t* rdram, uint32_t car_state, int view);
int rush2_online_body_kind(uint8_t* rdram, recomp_context* ctx, int car, int kind);
int rush2_online_wheels_shown(uint8_t* rdram, uint32_t physics_car, int view, int shown);
uint32_t rush2_online_arrow_views(uint8_t* rdram, int car, uint32_t flags);
int rush2_online_arrow_view(uint8_t* rdram, int view);
int rush2_online_hud_views(uint8_t* rdram, int views);
void rush2_online_hud_time_digits(uint8_t* rdram, int player);
uint32_t rush2_online_view_record(uint8_t* rdram, int view, uint32_t record);

// Chase Camera and LOOK BACK (src/camera.cpp).
void rush2_attract_camera_view(uint8_t* rdram, recomp_context* ctx);
void rush2_attract_camera_cycle(uint8_t* rdram, recomp_context* ctx);
void rush2_look_back(uint8_t* rdram, recomp_context* ctx);
int rush2_online_engine_start(uint8_t* rdram, recomp_context* ctx);
uint32_t rush2_online_engine_car(uint8_t* rdram, int slot, uint32_t physics_car);
// func_803B7018 at 0x803B7110: the circuit menu set 0x800D3E40 to the profile's circuit block ($t8); a machine
// following an online host's circuit keeps its copy (src/online/online_game.cpp).
void rush2_online_circuit_block(uint8_t* rdram, recomp_context* ctx);
// func_800A8D1C at 0x800A8D8C (a circuit race's results): the same, after it pointed 0x800D3E40 at the profile again.
void rush2_online_circuit_results(uint8_t* rdram);
// circuit_generate_800A7DCC at 0x800A7E40: the same ($s7 the block).
void rush2_online_circuit_generate(uint8_t* rdram, recomp_context* ctx);
// Entry and return of the per-player sound effects (car_sound_update_all_80067358 and the functions it calls): online
// their player count is the same on every machine (src/online/online_game.cpp).
void rush2_online_sim_players_begin(uint8_t* rdram);
void rush2_online_sim_players_end(uint8_t* rdram);
// car_state_draw_objects_80082D14 at 0x80082E64, after it set the part animation switch 0x8011748C from the last
// frame's GPU time: online it is the same on every machine (src/online/online_game.cpp).
void rush2_online_draw_detail(uint8_t* rdram);
// car_sound_update_all_80067358 after its loop (0x80067A88): online the race's other human cars' stunt scoring and
// wheel dust; car_sound_slots_init_8006722C's entry: their stunt state cleared (src/online/online_game.cpp).
void rush2_online_extra_player_effects(uint8_t* rdram, recomp_context* ctx);
void rush2_online_stunt_reset(uint8_t* rdram, int car);

#ifdef __cplusplus
}
#endif

#endif
