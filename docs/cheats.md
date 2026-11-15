# Rush 2 cheats

What each of the game's cheats does, traced from the code. The cheat menu and how the recomp drives it are described
at the top of src/cheats.cpp.

## In-game behavior

Each cheat checked in a test race (2026-11-14, `tools/test_race.py` with `--config cheats.ID=...`; a two-choice
cheat is a checkbox and takes `true`, not its label). `RUSH2_CS_PLACE` put the car where a test needed it: 200 ft/s
sideways from the Las Vegas grid (-527.6, -33, -4164.7, heading 0,1) wrecks it; the Lower Manhattan subway rats are
at x -1438..-1344, z -532 and a cone row at x -2961..-3077, z -780; Las Vegas has a dirt bank at (-1030, -35, -4100).
Placing at a race clock under about 4 s lands in the countdown, where the car is held (good for parked views; a car
placed after GO on Las Vegas was put back near the start).

| Cheat (state byte) | Seen in game | Code |
|---|---|---|
| No Car Collisions (0x8011249A) | The player drove through the whole grid at 100 mph. | `car_collide_with_cars` (0x8006F3C0) is skipped for every car (not in mode 2). |
| Gravity (0x800E7D22) | Fall acceleration 26 / 32.2 / 48.4 / 64.5 ft/s² (0.8x, 1x, 1.5x, 2x). | `race_setup` (0x800A5ADC) sets gravity 0x80110018 from the f32 table at 0x800CB580. |
| Cone Mines (0x8010C166) | Cones are drawn as spiked mines; driving into one wrecks the car. | Cones get `breakable_behavior_g_hit` (0x8005EF74): car +0x648 = 1, cone hidden. |
| Car Mines (0x8010C167) | Cones are mines; the car that hits one becomes a spiked mine (state +0x34A = 1) and keeps driving; a computer car that touched it was wrecked. | `breakable_behavior_g_step` (0x8005ED38) sets +0x34A; `car_car_collision_response` (0x8006E2A8) wrecks the other car and clears it. |
| Burning Wreck (0x800D401C/D, per player) | The player's car starts as the wreck shell (flames and smoke, or charred without) and can't drive: 3 ft with the gas held. | Race setup (0x8009E8DC) sets the wreck flag 0x10 in +0x7F4; `car_wheel_forces_drag` (0x8006AFD8) then applies the wreck's drag. Original behavior. |
| Upside-Down (0x800D0171) | The 3D view and the track map are upside down; the same stick input turns the other way. | `view_fog_setup` (0x8007C624) rolls the camera; `input_steering_from_stick` (0x80076694) negates steering. |
| Disable Auto-Abort (0x8010C168) | A parked player was put back on the course after about 25 s; with the cheat it stayed until time ran out. | `race_stuck_wrongway_update` (0x8008CDA4) checks drones and human cars alike. |
| Super Speed (0x80111950) | From a grid start with the gas held (no RUSH2_CS_PLACE, which resets the matrix it scales): the player's car 126 mph at 8 s instead of 113, about 130 at its peak instead of about 118, the same at DIFFICULTY 0 and 5 (`RUSH2_TEST_RACE_DIFFICULTY`). Drones' lap average only 1-3 mph higher: the AI drives to its lanes' speeds. | Every car's orientation matrix is set to scale 1.00025 at race start (`car_reset_state_for_race`, 0x8008E050) and renormalized to it (0x80069330) instead of to 1; forces and velocities go through that matrix. |
| Inside-Out Car (0x801174E0) | Only the player's car is drawn inside out. | `mirror_state_update` (0x800776DC). |
| No Damage (0x8011CDB0) | The crash still wrecks the car; the panel dent bits (+0x7F4 top bits, 0x50000000 in the test) stay clear. | See [No Damage](#no-damage). |
| Invincible (0x80119634) | The 200 ft/s crash leaves the car whole; Suicide Mode contact doesn't wreck it; a Killer Rat still does. | See [No Damage](#no-damage). |
| Invisible Car (0x8010C170) | Every car is hidden, computer cars too. | `car_model_update_parts` (0x80059D24). |
| Invisible Track (0x8010C178) | The track is hidden; cars and sky stay. | `pvs_apply` (0x8007C27C). |
| No Brakes (0x80125C64) | The player's brake input stays 0 and the car coasts; computer cars set their own pedals. | `car_set_pedals` (0x80076854) skips the brake. |
| Super Tires (0x8010D3A6, on = 2) | On the Las Vegas dirt bank the car bogged down to a stop without it and drove out at 70 ft/s with it; on Honolulu's dirt road (1919, 205, 6139) it kept slightly more speed (126 vs 117 ft/s). | `car_wheel_forces_drag`: rear wheels on dirt (1) or grass (2) get 0.1 of the drag; off-road tires lose their pavement drag. Every car. |
| Mass (0x8010C179) | Every car accelerates slower: 12% (Heavy) and 22% (Massive) less distance in 9 s; drones' lap average 128 to 122 mph. | `car_type_get_mass` (0x8008DB04) adds value x 1000 lb to every car. |
| Suicide Mode (0x80119628) | On: computer cars bumping on the grid all blew up. Humans: only contacts with the player's car. Invincible still spares the player. | `car_car_collision_response` wrecks both cars. |
| Do the Dew (0x8010C17A) | Las Vegas key 14 (630, 4.7, 2496), seen from RUSH2_CS_PLACE 630.2,7,2526.3,0,-1,0,0.5: a key normally, a Dew can with the cheat. | `track_object_spawn_key` (0x800BC3F8) gives keys the Dew model 0x756. |
| Killer Rats (0x8010C17C) | Giant white rats in the Lower Manhattan subway; touching one wrecks the car, Invincible or not. | See [Killer Rats](#killer-rats). |
| Stunts on All Tracks (0x8010C200) | The stunt score box shows on Las Vegas. | `hud_stunt_box_visible` (0x80060E34). |
| Resurrect in Place (0x8010C210) | After a wreck the car comes back 10 ft above where it wrecked. Human cars only. | `respawn_place_car` (0x80090A40): car +0x664 = position + 10 ft. Without Levitation only once per 2 s (time per car at 0x800C44AC); a second wreck sooner respawns normally. |
| Levitation (0x8010C254) | With Resurrect in Place, pressing ABORT (C-up) repeatedly in the air lifts the car higher and higher. Without Resurrect in Place it does nothing. The Cheats tab offers it as Resurrect in Place's third choice. | `respawn_place_car` reads it only after RESURRECT IN PLACE (0x80090B58-0x80090B68) and then skips the 2 s wait between in-place respawns, each of which puts the car 10 ft above where it is. |
| No Race Timer (0x800D9E88) | The race ran 67 s past GO without timing out (normally about 46 s). | `race_start_timer` (0x800AE670), `hud_widget_cb_time_left` (0x800B94C0). |
| New York Cabs (0x800D9E89) | Every drone a taxi on Lower Manhattan; no change on Las Vegas. | Only on track 1 or 3 (0x8010C3F0). |
| Frame Scale (0x8010C43C) | 1 squashes the player's car flat, 3 stretches it tall; computer cars unchanged. | Scales the car matrix's Y column by 0.6 / 1.2 / 1.5 (0x800BFC80). |
| Tire Scaling (0x8010C3D4) | TIRE SIZE F and TIRE SIZE R rows at the bottom of car select. | `carselect_build_list` (0x803B81F0). |
| Fog Color (0x8010C440) | Distant scenery (and on Manhattan the sky) tinted red; on Las Vegas at Double draw distance the haze is too far out to see. | `race_setup` uses the value (12-19) instead of the track id to index the fog colors at 0x800C1D7C. |
| No Subways or Buses (recomp) | Train count 0x800D4E74 is 6 on both Manhattan tracks, 0 with it. | `subway_init` (0x800A4210). |

## Killer Rats

Cheat item 18, state byte `0x8010C17C` (`cheat_option_k`). Cleared by the boot defaults (`save_set_boot_defaults`,
0x800A00DC, at 0x800A03EC) and read in two places.

**Which tracks.** The rats are the `RATCONE` placement records (behaviour 0x777, model `RATCONEO3`, textures
`RATCONEA` = grey rat, `BADRAT` = pale rat). Only NYONE (Lower Manhattan, 23 records) and NYTWO (Upper Manhattan, 22)
place them (`tools/rush2049/placement.py --verbose`). The cheat changes nothing on any other track.

**At track load**, `track_object_spawn_ratcone` (0x800BC888) allocates the rat's breakable, sets its id 0x777, and
branches on the byte:

| | Off | On |
|---|---|---|
| hit callback (+0x58) | `breakable_behavior_g_step` (0x8005ED38) | `breakable_behavior_g_hit` (0x8005EF74) |
| after-hit update (+0x5C) | `breakable_behavior_follow_pair` (0x8005EC14) | none |
| size | as placed | `mat3_scale(pose, pose, 4.0)` (0x8005A340) on the node's pose: 4 times as big |
| texture | `RATCONEA` | `track_behavior_ratcone_o3` (0x800AF460) swaps it for `BADRAT` |

The texture swap: `track_behavior_ratcone_o3` stores the `RATCONEA` and `BADRAT` texture handles in 0x80111880 and
0x80111938, then runs `mirror_winding_walk` (0x80077518) on `RATCONEO3` in mode 10, whose bit 8 replaces every
display-list word equal to 0x80111880 with 0x80111938.

`math_mat3_scale` (0x8005A340; same code in SF Rush at 0x80068AA0 and Rush 2049 at 0x8008B32C) was once listed as a
word memcpy. It multiplies the nine floats of a 3x3 matrix by the float in `$a2`.

**Every frame**, `breakables_update` (0x8008A01C) sets node flag 0x8000 on breakables 0x717 (meter) and 0x777 (rat),
except a rat while the cheat is on (0x8008A0EC). With 0x8000, `scene_walk` (0x8007B518, branch at 0x8007B860)
builds the node's matrix from 0x800E7C78 instead of its pose: `view_basis_matrix_init` (0x8007BF2C) makes that a
yaw-only rotation from the camera's look-at matrix (0x800E7CC8), so the node is an upright card turned to face the
camera, drawn with its pose's position only (so without the 4x scale). With the cheat on, the rat is drawn with its
scaled pose and placed orientation instead.

**The hit.** `breakable_update_all` (0x8008BA68) runs for each car whose driver byte (0x800D9C10 + slot * 9 + 8) is 0
(computer) or 8 (human), from `race_frame_update` (0x800A3128). For each active breakable it does the sphere test
(0x8008B95C) and the car box test (0x8008B0CC), then calls +0x58(breakable, car). With the cheat on that is
`breakable_behavior_g_hit`: if the car's wrecked byte (car +0x648) is 0 and the id is 0x777, it plays the rat's
object sound (`sound_play_object_effect`, 0x8005E388) and sets car +0x648 = 1, wrecking the car. The rat is not
hidden or knocked away, so it stays on the road.

So with the cheat: the rats on the two Manhattan tracks are giant pale rats, and any car, human or computer, that
touches one is wrecked. Nothing makes the rats move. Because the spawner reads the byte at track load, turning it on
or off mid-race only toggles the camera-facing flag; the full effect starts with the next race.

## No Damage

Cheat item 9, state byte `0x8011CDB0` (`cheat_no_damage`); Invincible is item 10, `0x80119634` (`cheat_invincible`).
Three places raise a car's panel damage bits (physics car +0x7F4, 2 bits per panel): `car_collision_damage`
(0x8006B730, at 0x8006B914), `car_crash_damage_stage` (0x800715E8, at 0x8007189C) and `car_race_start_pose_setup`
(0x8009A264, at 0x8009AA68). Each makes the same test: if the car's kind (+0x7E8) is 2 (human) and Invincible or No
Damage is nonzero, the damage write is skipped; a computer car (kind 1) is always damaged. So the original No Damage
only spares the players' cars.

Invincible also keeps a human car from being wrecked by a crash: where `car_car_collision_response` (0x8006E2A8, at
0x8006EA4C and 0x8006EB94) and `car_crash_damage_stage` (at 0x80071750 and 0x800717A0) wreck a car (+0x648 = 1),
a human car gets `Invincible ^ 1` instead. Nothing else reads it, so water and the out-of-bounds floor (the wheel
probe's `car_wreck_trigger`) and the Killer Rats still wreck an Invincible car.

The recomp's **No Damage's Cars** select (beside No Damage on the Cheats tab) widens it. A hook after each kind load
(us.toml) calls `rush2_cheats_no_damage` (src/cheats.cpp) with the physics car (0x800F5470 + car * 0x81C):
**Player** leaves the game's test; **All Cars** spares every car while No Damage is on; **New Cars** spares only the
cars whose damage is drawn as the port's generated dents (`rush2::car2049::generated_damage`: Rush 2049, California
Speed and mod cars, and mod liveries), so Rush 2's own cars, the player's included, take damage. Invincible still
spares a human car in every mode.
