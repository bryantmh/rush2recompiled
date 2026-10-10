# Original game fixes

Bugs in the original Rush 2 (US) that the recomp fixes (src/game_fixes.cpp), always on unless a section says it has
an option, and a few small gameplay options that live in the same file. Each is a hook into the game's own code.

## Mountain Dew paint

GitHub issue #5. The car select frame (func_803B9478) works out every frame, at 0x803BA644-0x803BA7E4, whether
slot `$s7`'s cursor row can be changed for the slot's car type, into the byte 0x803CB3AC[slot]. Left and right do
nothing on a row while it is 0 (0x803B983C). By option id (0x803CB3B8[row]: 1 TRANSMISSION, 2 MAIN COLOR,
3 ACCENT COLOR, 4 STRIPE, 5 STRIPE COLOR, 8 ENGINE):

| Car type | Rows blocked |
|---|---|
| 16 TAXI | 3 |
| 18 FORMULA | 4, 5 |
| 20 ROCKET | 1, 4, 5, 8 and the id in `$s3` |
| 21 DEW | 2, 3, 4, 5 |

The car paint builder (func_8008582C) still tints the Dew. Its palette class table at 0x800C5698 (the other cars use
0x800C5670) gives entries 1-31 MAIN (flags 0x22) and 33-63 ACCENT (0x42) like every car, and fixes 64-255. That is
why computer opponents in a Dew showed other colors. The builder skips only the stripe, for types 18, 20 and 21
(0x80085CA8).

The fix, a hook at 0x803BA7E8, sets the byte back to 1 on MAIN and ACCENT for the Dew. STRIPE and STRIPE COLOR stay
blocked, because the builder would ignore them.

## Engines after time out

GitHub issue #4. In a race (game state 3, func_800AE670 at 0x800AEC14), once the time left is down to 0.5 s the game
sets the out of time flag 0x800FAE98, plays sound 0x5A and sets the engines-off byte 0x800E7BCE (0x800AEF94).
func_800650DC then gives the engine sounds rpm 0 (0x800651D0).

A checkpoint reached while coasting adds time, and the next frame clears 0x800FAE98 (0x800AEDC4). Nothing clears
0x800E7BCE until the next race setup (func_800A5ADC at 0x800A6134), so the engines stayed silent for the rest of the
race. Rush 2049 fixed this. The hook at 0x800AEDC0 clears the byte along with the flag.

## Finish place and DEATHS

User report. Always on: a vanilla bug, so it has no option.

With DEATHS on, the race global 0x8010BC98 (s8, copied from the track select struct 0x800D5760 + 0x11 by
func_80094698 at 0x80094974) is non-zero. `race_car_finish_or_respawn_handle_8009E180` (`$a0` = respawn case, `$a1`
= car) treats case 2 as a car out of deaths: when the car state's out byte (0x801124A0 + car * 0x354, +0x343) is 0,
it sets it to 1, clears car +0x648, and calls `race_car_finish_8008E8EC` with the race time. race_car_finish marks the
car finished (+0xEA, time +0xEC) and sorts every finished car by time, but gives a car with +0x343 > 0 the time
`0x800CFBE8 + (s32)(9 * 0x8010C430) - car state +0xFC`, so dead cars rank behind every real finisher. Nothing checks
whether the car had already finished, so a car that crossed the line and then wrecked was re-ranked into the last
place left.

The fix, a hook at the entry of func_8009E180 (`rush2_fix_finish_place`), changes case 2 to case 1 (a plain respawn,
which shares the case 2 path apart from the out of deaths block) when the car state's +0xEA is already 1.

Other readers of 0x8010BC98, not changed: car_wreck_trigger_8006E1A8 (0x8006E270), save_commit_settings_to_pak
(0x800A8C84), record_stats_race_end (0x800A946C), race_update_player_slot_state (0x800AB11C), hud_widget_cb_simple_reset
(0x800B9C48, the skull icon) and the options list (0x803C6844 / 0x803C6898).

## White accent on a white car

User report. `car_build_textures_8008582C` copies the car's base palette (CARPALETTE, 0x80119648) and, for the white
color (index 0), first brightens the color's ramp: MAIN COLOR's entries 1-31 when MAIN is white (0x80085898), else
ACCENT COLOR's entries 33-63 when ACCENT is white (0x80085960, an else-if). Each entry becomes a gray of its red
channel x 8 x 1.25 (at most 255). With both colors white the accent ramp was never brightened, so a white accent was
darker on a white car than on any other. The hook at 0x80085C14, where the branches join (0xA4($sp) = the palette
copy, 0xD3($sp) = MAIN, 0xD7($sp) = ACCENT), brightens the accent ramp then too.

## Rocket windshield

User report. `car_palette_classify_800854AC` sorts a car's palette entries into classes from a range table of 4-byte
entries {first, last, flags, 0}, ended by first > last. 0x800C5670 (every car but the DEW, which uses 0x800C5698):
0 fixed, 1-31 MAIN (0x22), 32 fixed, 33-63 ACCENT (0x42), 64-95 stripe stamped (0x10), 96-111 fixed, 112-143 0x10,
144-207 fixed, 208-255 0x10. The ROCKET's (type 20) container (asset 0x31) has no texture table: its 11 CI8 textures
load through the container's texture display lists (0x4830-0x4AF0). The windshield is near-black (entry 190,
CARPALETTE 8, 8, 8) except for one panel, a solid block of entry 17 framed by entries 179-185 in texture 6 (32 x 64;
all 184 of its entry-17 texels are the block), which took the MAIN COLOR. Entry 17 is body paint in the other
textures, so its class can't change. At boot (`rush2_heap_init`, src/assets.cpp) `fix_rocket_windshield`
replaces the asset with a copy whose texture 6 uses entry 190 for those texels.

`rush2::fix_car_models` (called from `rush2_heap_init`) makes this fix and the two below. Car model units are 1/16
ft. `tools/rush1/cartex.py`'s `meshes()` lists a car's triangles by model for checking these.

## Formula suspension

User report. Always on: a vanilla bug (SF Rush has the same model and tile), so it has no option.

The FORMULA's (type 18, asset 0x2F) wishbones are one 64 x 32 CI8 texture (texels at 0x828, a V wide at texel column 0
with its apex at 63) on two tent-shaped strips across the car, front in FORM1D0_FR1 (z 54-63, y 3-11) and rear in
FORM1D0_RR1 (z -81 to -90, y 0-8), loaded by the texture list at 0x3070. s runs from -62.6 at x = +38 to 63.5 at
x = -38, meant to mirror at the middle so both sides run from the chassis to the hub, but the render tile wraps
(G_SETTILE w1 0x00014060: cms 0, masks 6). So the right-hand wishbones ran backwards, apex at the chassis and wide end
at the hub. The fixed copy sets the mirror bit (0x100) on that tile.

Every car's wheels are drawn at the descriptor's wheel positions (+0x7C: x +-2.75, z +-3.925 ft in the FORMULA's
descriptor 7, which most cars share), copied to car state +0xA8 by `car_race_start_pose_setup_8009A264`. At 16 units
per foot that is z = +-62.8: the front wheels sit on the front arms' hubs (z about 58), but the rear wheels sat 22
units ahead of the rear arms' hubs (z about -85, under the wing) and inside the sidepods (x up to 40 back to z -65).
`carselect_preview_update_8005A598` places the wheel nodes (loop at 0x8005A8B8, `$a2` = wheel, 2 and 3 rear, `$t1` =
physics car, type at +0x7EA): the hook at 0x8005A8F8 (`rush2_fix_formula_rear_wheels`) gives the FORMULA's rear wheels
z = -85.25 / 16 ft. Only the drawing moves; the physics wheels stay at -3.925.

## Stallion spoiler

User report. Always on: a vanilla bug, so it has no option.

The STALLION's (type 14, asset 0x2B) body is symmetric about x = 0 (sides +-50, rear deck +-16 at z -104), but its
spoiler runs from x = -36 to 39, trailing edge -13 to 17 (center +2). The spoiler is in every TOP model (MUSTD0_TOP1,
MUSTD1_TOP1, MUSTD0_DTOP1, MUSTD0_D2TOP2), whose own vertex buffers hold nothing else behind z = -80. The fixed copy
moves those vertices 2 units left: trailing edge -15 to 15, tips -38 and 37.

## Stunt respawn gear

Option (General > Gameplay, Stunt Respawn in 1st Gear, off by default). `race_car_finish_or_respawn_handle_8009E180`
puts player 1's car (car table 0x800D9C10 + car * 9, +8 driver == 8) in 2nd gear (car +0x738 = 2, 0x8009E3E0) on
every respawn. The hook at 0x8009E3E4 sets 1st instead in stunt mode (track slot 11, which also hosts the Rush 2049
arenas and the battle arenas).

That alone showed 1st and then went back to 2nd (user report, 2026-10-10). After the respawn the wrecked car glides
to its respawn point: `car_wreck_steer_to_respawn_800754B4` counts car +0x6C8 up and ends the glide with +0x6C8 = -2
(0x80075650). Next frame `car_physics_tick_all_reset_8008E258`, for every car with +0x6C8 == -2, sets it to -1, resets
the car (`car_reset_state_for_race_8008E050`, ground contact, integration, engine rpm) and puts it in 2nd gear for
every car, not just player 1: +0x738 (the selected gear), +0x3E0 and +0x3DE (the gearbox's) = 2 at 0x8008E330-
0x8008E33C. A second hook at 0x8008E340 (`rush2_stunt_respawn_gear_reset`) sets all three to 1 for human cars
(physics car +0x7E8 == 2) in stunt mode.

Writers of +0x738: `player_join_init_car_80080524` (the buttons, every frame: reverse held -1, shift down/up step 1-4,
by speed after reverse is released), `car_zero_inputs_8008D7BC` (1), `car_init_setup_8008DBA0` (0),
`car_physics_tick_all_reset_8008E258` (2) and the respawn (2). `car_physics_inputs_80071A1C` copies it to the gearbox:
+0x3DE and +0x3E0 when car +0x3D8 is 0 (manual), only +0x3E0 otherwise (automatic picks +0x3DE itself). The HUD gear
(`hud_widget_cb_model_lookup_800B92F4`, 0x800B9328) reads +0x738.

## Keep music playing when paused

Option (Sound tab, off by default; src/music.cpp). `race_pause_menu_step_800AFD44` stops the music when the race
pauses (music command 0x40000000 queued at 0x800AFE40, after `func_80062FC4` at 0x800AFE38) and on CONTINUE restarts
the race's song from the top with `func_8008C370(2, MUSIC setting)` at 0x800B00B8, which picks a new song under a
shuffle. While the option is on, hooks around both calls (0x800AFE38-0x800AFE48 and 0x800B00B8-0x800B00C0) hold their
music commands: the stop becomes a volume update (0xC0000000) and the song choice does nothing, so the song plays on.
The option can be changed with the pause menu open: the hook at the pause step's entry (0x800AFD44) starts the race's
song at once (`func_8008C370`, as CONTINUE does) or stops it (command 0x40000000), and CONTINUE holds its restart
only while the song plays.
