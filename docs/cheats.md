# Rush 2 cheats

What each of the game's cheats does, traced from the code. The cheat menu and how the recomp drives it are described
at the top of src/cheats.cpp.

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
