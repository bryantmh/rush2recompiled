# Race cameras

Rush 2's race cameras, and the two camera features built on them (src/camera.cpp): the Chase Camera option and
the LOOK BACK binding.

## Camera modes

Each car state (0x801124A0 + car * 0x354) holds its view (+0x347, -1 for none) and its camera mode (+0x348; +0x349 a
copy the wreck camera returns to). cam_update_all_players_8009D9F4 changes the mode once per frame per view and
cam_update_dispatch_8009D02C runs it (jump table 0x800CFD88, modes 0-10):

| Mode | Camera | Code |
|------|--------|------|
| 0 | bumper: (0, 1.2 + height, 4.0) in the car's frame (0x800BFC58/5C) | 0x8009D0E8 |
| 1 | hood: (-0.55, 3.2, 0) less the lagging offset 0x800E7C38 + view * 0xC | 0x8009D17C |
| 2 | chase, at the view's chase height and distance | cam_update_chase_main_8009C228 |
| 3 | close chase: 5 lower and 4 nearer than mode 2 (0x8009C2BC) | cam_update_chase_main_8009C228 |
| 4 | wreck camera (set by 0x8008E5BC, which saves the mode in +0x349) | cam_update_mode_a_8009BE90 |
| 5 | orbit | cam_update_mode_orbit_8009BB9C |
| 6 | attract trackside camera at a random path point ahead | cam_update_random_orbit_attract_8009D858 -> cam_update_mode_d_8009BAC0 |
| 7 | race end (0x800A36FC) | cam_update_mode_c_8009B5B0 |
| 8, 10 | 10: attract front camera, ahead of the car looking back at it | cam_update_mode_e_8009C050 |

A race starts in mode 2 (func_8009E6DC at 0x8009E750); no mode is saved anywhere. The VIEW button (player record
+0x1A mask against +2 presses, 0x8009DD58) adds 1 to the mode at 0x8009DD6C and wraps at 4 (0x8009DD70); entering 2
calls cam_set_chase_mode_8009B480 and puts the chase camera (0x800E7B38 + view * 0x30, moved to 0x80240390; its
position at +0x24) back on the car, entering 3 calls cam_set_chase_mode_8009B480.

cam_set_chase_mode_8009B480 (car state) sets the mode to 2 (+0x348 and +0x349), clears the view's ground polygon
cache (0x800E7A00 + view * 0x40), sets 0x800E7BD0[view] = 1 and the chase height 0x800E7CC0[view] and distance
0x800E7D10[view] (moved to 0x802404B0 / 0x802404C0 for four players): 6 / 16 in a race (7 for car type 15). With
race_game_mode_800FAE6C set they are 6 / 20 while race_attract_mode_800E7BB0 is set and 12 / 30 once it is clear.
Modes 2, 3 and 10 read them.

## Attract demo cameras

The attract demo sets race_game_mode_800FAE6C and race_attract_mode_800E7BB0 (0x800AB9BC), and the race reset
(race_reset_hud_and_globals_8009FD68 at 0x8009FDB0) clears race_attract_mode_800E7BB0 again, so in the attract race
cam_set_chase_mode_8009B480 gives 12 / 30: the chase camera high above and far behind the car. While
race_game_mode_800FAE6C is nonzero cam_update_all_players_8009D9F4 rolls a new camera every 3 s (timers 0x800BFC74 /
0x800BFC7C per view): rng * 6 / 32768 picks, through jump table 0x800CFDC4 (0x8009DC38), 0 bumper, 1 hood, 2 or 3 the
chase camera other than the current one, 4 the front camera (cam_set_chase_mode_8009B480 then mode 10, 0x8009DCC0),
and 5 the trackside camera (mode 6, 0x8009DCD4).

Mode 10 (cam_update_mode_e_8009C050) passes the negated distance and the height in $f22 / $f26 to
race_transform_points_and_search_cars_8009B180, which places the camera ahead of the car along its velocity, looking
back at it, then cam_chase_position_compute_8009AC74.

## Chase Camera option

Gameplay tab, off by default. VIEW goes 0, 1, 2, 3, then the attract demo's high chase camera, then back to 0. The
high chase camera is mode 2 at height 12 and distance 30, told apart from mode 2 by those two values, so it needs no
state of its own: anything that sets the chase camera again (VIEW to 2 or 3, a new race) puts back the race's.

- The hook at 0x8009DE30 (the join where every path has the new mode in $s0 and the old one in $a0) turns VIEW's wrap
  from 3 into the high chase camera, set up as VIEW sets up mode 2 (cam_set_chase_mode_8009B480 and the chase camera
  put back on the car) with 12 / 30 written over the race's values.
- The hook at 0x8009DD6C, where a VIEW press adds 1 to the mode, makes a press in the high chase camera go on to 0
  (the join is too late to tell: entering 3 has already called cam_set_chase_mode_8009B480). It runs with the option
  off too, so turning the option off mid-race still leaves the high chase camera at the next press.

Both do nothing while race_game_mode_800FAE6C is set.

## LOOK BACK

A binding (Controller Setup screen, D-pad left / B by default; src/controls.cpp `look_back`). Every mode leaves its
camera in view_cameras_800E79D0 + view * 0x40 (rows = the camera's axes in world space, position at +0x24), which the
view setup (view_fog_setup_8007C624, pvs_apply_8007C27C) and the sky read. While the view's player holds LOOK BACK in
modes 0-3 that block is turned 180 degrees about the car's up axis (row 1 of the car state's matrix +0x24) through the
car's position: the chase cameras sit ahead of the car looking back at it, the bumper and hood cameras at its rear
facing backward.

The turn is only there while the frame is drawn. The hook at 0x8009DE58, right after cam_update_dispatch_8009D02C
returns, only notes that the view looks back. The block is turned at the start of the view's draw
(view_fog_setup_8007C624 entry, called from rush2_interp_view_begin so the cut detection sees the drawn camera) and
put back after the frame's views and HUD (end of rush2_hud_draw_end). A turn left in the block between frames made the
chase cameras jump between two places every frame.

Even with that, the chase cameras can't be turned from the block: while the car is off its path (physics +0x6C8 < 0,
car state +0x309 outside 0-0x14, race type not 3; 0x8009D4A0) the block goes through collision_ground_query_basis_8008BDC0
(0x8009D4F4), and after a frame drawn turned that query put the camera next to the car every other frame. So the chase
cameras are turned from their own state (0x800E7B38 + view * 0x30, moved to 0x80240390: axes, position at +0x24),
which the dispatcher copies into the block (0x8009D474) and which no drawing affects, and the turned camera has no
ground check: on a curve it can end up inside a wall.

The chase camera's state isn't touched, so letting go puts the camera where it would have been. Frame interpolation
takes the turn as a camera cut (more than 60 degrees, src/interpolation.cpp) and snaps.

The view's player is its record's port (+1); online, view i is this machine's port i.

### Look Back option

Gameplay tab, on by default, takes effect at once (`rush2::set_look_back`). Off, the hook at 0x8009DE58 never notes a
look back, as in Rush 2, and the Controller Setup screen leaves out its LOOK BACK row the next time it opens (the rows
are latched when it opens, docs/menus.md#controller-setup-screen). LOOK BACK also does nothing while
race_game_mode_800FAE6C is set (the attract demo).
