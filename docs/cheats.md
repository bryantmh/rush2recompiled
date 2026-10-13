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
