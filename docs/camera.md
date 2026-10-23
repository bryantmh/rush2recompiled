# Race cameras

Rush 2's race cameras, and the two camera features built on them (src/camera.cpp): the Attract Camera option and
the LOOK BACK binding.

## Camera modes

Each car state (0x801124A0 + car * 0x354) holds its view (+0x347, -1 for none) and its camera mode (+0x348; +0x349 a
copy the wreck camera returns to). cam_update_all_players_8009D9F4 changes the mode once per frame per view and
cam_update_dispatch_8009D02C runs it (jump table 0x800CFD88, modes 0-10):

| Mode | Camera | Code |
|------|--------|------|
| 0 | bumper: (0, 1.2 + height, 4.0) in the car's frame (0x800BFC58/5C) | 0x8009D0E8 |
| 1 | hood: (-0.55, 3.2, 0) less the lagging offset 0x800E7C38 + view * 0xC | 0x8009D17C |
| 2, 3 | chase (VIEW's third and fourth) | cam_update_chase_main_8009C228 |
| 4 | wreck camera (set by 0x8008E5BC, which saves the mode in +0x349) | cam_update_mode_a_8009BE90 |
| 5 | orbit | cam_update_mode_orbit_8009BB9C |
| 6 | attract trackside camera at a random path point ahead | cam_update_random_orbit_attract_8009D858 -> cam_update_mode_d_8009BAC0 |
| 7 | race end (0x800A36FC) | cam_update_mode_c_8009B5B0 |
| 8, 10 | 10: high chase camera | cam_update_mode_e_8009C050 |

A race starts in mode 2 (func_8009E6DC at 0x8009E750); no mode is saved anywhere. The VIEW button (player record
+0x1A mask against +2 presses, 0x8009DD58) cycles 0-3 and wraps at 4 (0x8009DD70).

cam_set_chase_mode_8009B480 (car state) sets the mode to 2 (+0x348 and +0x349), clears the view's ground polygon
cache (0x800E7A00 + view * 0x40), sets 0x800E7BD0[view] = 1 and the chase height 0x800E7CC0[view] and distance
0x800E7D10[view] (moved to 0x802404B0 / 0x802404C0 for four players): 6 / 16 in a race (7 for car type 15), 6 / 20
in the attract demo, 12 / 30 with race_game_mode_800FAE6C set outside it. Modes 2, 3 and 10 read them.

## Attract demo cameras

While race_game_mode_800FAE6C is nonzero (the attract demo), cam_update_all_players_8009D9F4 rolls a new camera every
3 s (timers 0x800BFC74 / 0x800BFC7C per view): rng * 6 / 32768 picks, through jump table 0x800CFDC4 (0x8009DC38),
0 bumper, 1 hood, 2 or 3 the chase camera other than the current one, 4 the high chase camera
(cam_set_chase_mode_8009B480 then mode 10, 0x8009DCC0), and 5 the trackside camera (mode 6, 0x8009DCD4).

Mode 10 (cam_update_mode_e_8009C050) passes the negated distance and the height in $f22 / $f26 to
race_transform_points_and_search_cars_8009B180, which places the camera along the car's velocity at that distance,
then cam_chase_position_compute_8009AC74.

## Attract Camera option

Gameplay tab, off by default. The hook at 0x8009DE30 (the join where every path has the new mode in $s0 and the old
one in $a0) turns VIEW's wrap from 3 into the high chase camera, set up as the attract demo does: it calls
cam_set_chase_mode_8009B480, writes the attract demo's height 6 and distance 20 and makes the mode 10. VIEW from 10
wraps to 0 as usual, so turning the option off takes effect at the next press.

## LOOK BACK

A binding (Controller Setup screen, D-pad left / B by default; src/controls.cpp `look_back`). Every mode leaves its
camera in view_cameras_800E79D0 + view * 0x40 (rows = the camera's axes in world space, position at +0x24), which the
view setup (view_fog_setup_8007C624, pvs_apply_8007C27C), the sky and the sound read. The hook at 0x8009DE58, right
after cam_update_dispatch_8009D02C returns, turns that block 180 degrees about the car's up axis (row 1 of the car
state's matrix +0x24) through the car's position while the view's player holds LOOK BACK in modes 0-3 or 10: the
chase cameras sit ahead of the car looking back at it, the bumper and hood cameras at its rear facing backward.

The chase camera's own state (0x800E7B38 + view * 0x30) isn't touched, so letting go puts the camera where it would
have been. Modes 2 and 3 keep the camera out of the ground with collision_ground_query_basis_8008BDC0 while the car is
off its path (physics +0x6C8 < 0, car state +0x309 outside 0-0x14, race type not 3; 0x8009D4A0); the turned camera
does the same with a polygon cache of its own, so the chase camera's (+0x30) stays put. Frame interpolation takes the
turn as a camera cut (more than 60 degrees, src/interpolation.cpp) and snaps.

The view's player is its record's port (+1); online, view i is this machine's port i.
