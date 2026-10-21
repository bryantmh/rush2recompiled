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

## Hood view front wheels

Option (General > Gameplay, Hood View Front Wheels, on by default, hot-swappable). Rush 2 hides a car's own wheels in
its bumper and hood cameras, which leaves the open-wheel cars' front wheels and arms missing from beside the nose. On,
the FORMULA (18), MOUNTAIN DEW (21) and 2049's FORMULA 1 (23) show their front wheels in the hood camera, moved to sit
beside the hood's nose, and the FORMULA's hood also draws its front arms.

Visibility. `carselect_preview_update_8005A598` runs once per wheel node per frame (node record `$a0`, kept in `$t0`:
+0xE car, +0x10 wheel, 0 and 1 front). After `node_reset_visibility_800590AC` (0x8005A728) it sets `$s0` = camera
mode (car state +0x348) >= 2 and calls `node_swap_visibility_8005A528` (0x8005A78C) with the car's view (+0x347): for a
view >= 0 it shows the node in that view (`node_show_80058ED4`, mask `1 << view`) when `$s0` is set and hides it there
(`node_hide_80058D1C`) otherwise. The hook at 0x8005A774 (`rush2_hood_front_wheels`, after the online one at
0x8005A770) sets `$s0` for wheels 0 and 1 in camera mode 1.

Centering. The hood camera (`cam_update_dispatch_8009D02C`, mode 1 at 0x8009D17C) sits at (-0.55, 3.2, 0) ft from the
car (0x800BFC60-68) less the view's lagging offset (0x80240480 + view * 0xC), so 8.8 units left of the middle. The
open-wheel cars' hoods were modeled shifted with it, so the nose looks centered: `FORM1H0_L` / `H0_R` meet at x = -6
(x -41 to 28), `DEWH0_L` / `H0_R` at -8 (-40 to 24), and 2049's `CAR1HOOD` has its nose tip at x = -8 (-68 to 52).
Closed cars' hoods are centered (`PICKUPH0_L` / `H0_R` meet at 0). The body and the wheel positions (car state +0xA8,
the descriptor's +-2.75 ft) are centered on 0, so the wheels came out off-center against the hood. The hook at
0x8005A8F4 (`rush2_hood_front_wheels_x`, after the wheel's x is stored at 0xCC($sp), passed by pointer at 0x8005AC40)
moves the front wheels by the hood's shift / 16 ft while the hood is drawn (mode 1, one player: s16 0x8010C3E2 < 2).

FORMULA arms. Its front wishbones are in the front panel `FORM1D0_FR1`, which the hood camera hides with the rest of the
body: after the G_DL to the wishbone's texture load list (0x3070, texels 0x828) its list sets render mode `C8113278`
and the car combiner `FC1216AC F0FFFE38` and draws four G_TRI2s over vertices 14-19 of its first G_VTX (a tent across
the car, x -38 to 38, y 3-11, z 54-63). At boot `capture_formula_arms` records those commands, the vertices' file
offsets and the hood lists' (`FORM1H0_L`, `FORM1H2_L`), and the FORMULA's asset copy gets 0x200 bytes of room at its end
(the car slot size 0x8001CF28 grows to fit if needed). Each frame, for a race car with a view, `update_formula_arms`
finds the hood halves' model records (part handles 31 and 32 of car record 0x8010C480 + car * 0x4E; handle >> 10 =
model slot at 0x80118D78 + slot * 8, & 0x3FF = record, 0x34 bytes, LOD 0 list at +0xC). With the option on and the car
the FORMULA it writes, in that room of the loaded car, a list that calls the hood's own list and then draws the arms
(their vertices copied from the loaded car, x moved by the hood's shift, G_TEXTURE on, the texture load list, the
captured commands), and points the record's LOD 0 at it; off, it points it back. The record holds a KSEG0 address but
the pointers inside a list are physical (`00624F90`); a list in `recomp::alloc` memory (above 16 MB) froze the game in
the hood camera, which is why the lists live in the loaded car. The arms run at axle height, so in the hood camera only
their top edge shows along the bottom of the screen, meeting the wheels.

The DEW's hood model has its front arms in it already. In the same loop, wheel 0 in hood mode also feeds the hood
camera's bob (0x80240450 + view * 4, 0x8005A900-0x8005AA1C), which this leaves alone.

## Stallion spoiler

User report. Always on: a vanilla bug, so it has no option.

The STALLION's (type 14, asset 0x2B) body is symmetric about x = 0 (sides +-50, rear deck +-16 at z -104), but its
spoiler runs from x = -36 to 39, trailing edge -13 to 17 (center +2). The spoiler is in every TOP model (MUSTD0_TOP1,
MUSTD1_TOP1, MUSTD0_DTOP1, MUSTD0_D2TOP2), whose own vertex buffers hold nothing else behind z = -80. The fixed copy
moves those vertices 2 units left: trailing edge -15 to 15, tips -38 and 37.

## Stunt respawn gear

Option (General > Gameplay, Respawn in 1st Gear, off by default; named Stunt Respawn in 1st Gear and limited to
stunt mode until 2026-10-13). `race_car_finish_or_respawn_handle_8009E180`
puts player 1's car (car table 0x800D9C10 + car * 9, +8 driver == 8) in 2nd gear (car +0x738 = 2, 0x8009E3E0) on
every respawn. The hook at 0x8009E3E4 sets 1st instead, in every mode (stunt and battle arenas, practice, races).

That alone showed 1st and then went back to 2nd (user report, 2026-10-10). After the respawn the wrecked car glides
to its respawn point: `car_wreck_steer_to_respawn_800754B4` counts car +0x6C8 up and ends the glide with +0x6C8 = -2
(0x80075650). Next frame `car_physics_tick_all_reset_8008E258`, for every car with +0x6C8 == -2, sets it to -1, resets
the car (`car_reset_state_for_race_8008E050`, ground contact, integration, engine rpm) and puts it in 2nd gear for
every car, not just player 1: +0x738 (the selected gear), +0x3E0 and +0x3DE (the gearbox's) = 2 at 0x8008E330-
0x8008E33C. A second hook at 0x8008E340 (`rush2_stunt_respawn_gear_reset`) sets all three to 1 for human cars
(physics car +0x7E8 == 2).

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

CONTINUE now restarts the same song: `rush2_music_continue` hands the race's song (the one `func_8008C370` last
started, or a skip's pick) back to the next `func_8008C370(2, ...)` call, which plays it instead of choosing again
(src/music.cpp `forced_song`). A new race or RESTART still chooses.

## Pause menu extras

Option (General tab, Gameplay, on by default; src/pause_menu.cpp, include/pause_menu.h). Rush 2's pause menu:

- `race_pause_menu_step_800AFD44` runs it. State `0x8002305C` (s8): 0 racing, 1 the list, 2 OPTIONS, 3 CONTROLS,
  4 AUDIO, 5 and 6 RESTART's and QUIT's ARE YOU SURE. Cursor `0x80119635` (s8): 0 CONTINUE .. 5 QUIT, reset to 0 when
  the race pauses; up/down wrap it through 0-5, A on 1-3 enters state cursor + 1, on 4/5 the confirm screens, and B,
  START or A on CONTINUE resume. The list's input is read per player at 0x800B0060 with `$a0` = buttons pressed and
  `$v0` = B or START.
- `hud_setup_widgets_pause_like_800AFB50(_, state)` stores the state and builds its widgets; the list's come from the
  table `0x800BF03C` (13 records of 0x28 bytes: +0 model name, +4 x, +6 y, +0x20 update `func_800B558C`, +0x24 kind):
  the PAUSED title (kind 0x80), then per row its image twice, kinds 0x89 + row and 0x81 + row. Rows sit 15 pixels
  apart from y 93. `func_800B558C` centres each on x 160 and shows a half of the row image: kind & 0xF == cursor + 1
  is the cursor's row, whose yellow half flashes (alpha 0x7F-0xFF in steps of 8 a frame, `0x800D3A5C`/`0x800D3B20`).
- The row images (PAUSECONTINUE .. PAUSEQUIT, PAUSEYES/NO/SURE, PAUSED) are 4-bit color indexed images in assets
  22-24, stored upside down: rows 0-9 the grey half, 10-19 the yellow half; color 1 is the one texel outline, 2 grey,
  3 yellow (palette shared by the rows). In RDRAM they are reached through the HUD widgets' image pointers (+4).

The extras:

- **SKIP TRACK** between AUDIO and RESTART. Before the widgets are built (hook at 0x800AFB50) the table's rows are
  moved to y 85-175 to make room; the option off puts back 93-168. The row has no widget: its label is drawn after the
  HUD's widget loop, built at run time from the row images' own letters (S, T, R, A from RESTART, I and C from
  CONTINUE, P from OPTIONS, split by connected shapes) plus a K drawn to match, outlined like the rows. On SKIP TRACK
  the cursor is 6, which no widget's kind matches. The input hook at 0x800B0060 moves the cursor over the seven rows
  itself (clearing up/down so the game's 0-5 wrap never runs) and takes A on SKIP TRACK. Skipping picks another song
  of the race's game (any game under Shuffle All, or when the game has no other). With the music playing in the pause
  it starts at once through `func_8008C370`; stopped, it becomes the song CONTINUE starts.
- **Song box**, bottom right: the race's song with a note icon, and its game and courses below, which scroll like a
  music player's when longer than the box (a scissor around the line, `rush2::hud::push_scissor`).
- **Collectibles box**, top right, one player races only: player 1's finds on the track as Rush 2049's HUD shows its
  coins, a spinning model beside each count: Rush 2's key `KEYO1` and Dew can `DOTHEDEWO1` (asset 18, loaded for
  every race), SF Rush's key `KEYL1` and Rush 2049's `GOLDCOING_COIN` / `SILVERCOINS_COI` (converted into the track's
  container). A model is found with `func_8005BE3C`; its list is the first LOD of record `*(0x80118D78 + slot * 8) +
  index * 0x34` (+0xC). It is drawn from the 2D display list (`rush2::hud::call_list`, which saves and restores the
  render state, viewport and projection) under its own orthographic projection in a viewport anchored to the window's
  right edge (`gEXViewport`), depth just past the near plane, fog factor 0 (`func_8007C624` sets fog again for every
  view). Afterwards the 2D image loader's cache `0x80119638` is cleared: `func_80055F78` skips loading the image it
  loaded last, so text after a model was drawn from the model's texture.
- Both boxes are 8 pixels from the screen's edges and anchored to the window's right edge. Text printed in a race is
  normally queued (text scale `0x800BEF48` below 0 sends `func_80072DA4` to its queue); the boxes set it to 0 while
  printing so `func_80072A38` draws at once and takes the anchor, and lift the text clip inset (`0x80023058`, normally
  cutting text off past x 308), then put both back.

## Sky flicker

The sky's world polygons (flag 0x2000) draw at the primitive depth, which puts RT64's near clip on the camera plane,
so sky triangles reaching beside or behind the camera stretch or drop out. `rush2_interp_poly_end`
(src/interpolation.cpp, hooked at 0x8007D890 in `func_8007C624`) clips them at w = 256 (16 ft in front of the camera,
in polygon units of 1/16 ft) itself and redraws them from a side display list.

- The clip w is the polygon vertex times the root modelview times the projection stack. `func_8007C624` loads the
  perspective at 0x8007CB88, then multiplies the view's rotation into the projection at 0x8007CED0 (`guLookAtF`
  with the eye at the origin); the root modelview (0x8007CF24) is the identity or an X mirror, and the polygon
  vertices are camera-relative in world axes (0x8007D260). Leaving the rotation out measured a fixed world axis.
- **The dome was hidden.** `sky_setup_800A45A8` gives the dome prim depth 0x7FFF. RT64 turns that into exactly 1.0
  (`(z & 0x7FFF) / 32767`, rt64_rdp.cpp), the same as the game's Z clear (0xFFFC, `D16::toF`). The sky draws after
  the world with Z_CMP and RT64 compares with LESS, so every sky pixel failed and the fog colour showed instead of the
  SKY01/SKYFOUR clouds (Rush 2 and SF Rush tracks; 2049 tracks draw a sky model). `rush2_sky_depth` (hooked at
  0x800A48F8, where `$s7` = 0x7FFF) makes it 0x7FFE (0.99994), which passes against the clear and stays behind
  anything the far plane lets through.
- The triangles are clipped under this frame's plane and the last frame's (same camera generation). RT64 blends the
  projection element by element in its interpolated frames, so w there lies between the two; clipping against
  only the current frame let sky vertices swing behind the camera in fast turns (the race start flyover turns about
  15 degrees a frame).
- **Fixed vertex slots.** RT64 interpolates a draw's vertices between frames by index, only when the vertex count
  matches the last frame's (`GameFrame::matchScene`, lib/rt64 rt64_game_frame.cpp). Sutherland-Hodgman clipping gave
  a piece 3 to 5 vertices per triangle in an order set by which corners were cut, so whenever the counts happened to
  match, unrelated corners were paired and whole triangles swung across the sky in the interpolated frames (large
  triangles flickering now and then on every dome track). `clip_tri_slots` now clips every dome triangle, cut or not,
  into six slots: slot 2i is vertex i, or where the edge from its previous neighbor leaves the kept region when it is
  cut, slot 2i + 1 the same toward its next neighbor; duplicates where nothing is cut. Count and order never change
  and each slot moves continuously, so the pairing is always right. Drawn as a fan of four triangles per slot set
  (two G_TRI2).
- **The clip distance.** The dome is not a sphere around the camera: `render_sky_cloud_scroll_8008918C` moves it with
  the camera in x and z only, and its vertex table (0x800C1AA4, 25 vertices, 24 pieces indexed by 0x800C1CB4) is a
  flat lid at a fixed 900 ft world height out to a 725 ft radius, sloping to 600 ft at 1450 ft and 300 ft (alpha 0,
  alphas at 0x800C1C98, texture coordinates at 0x800C1BD0) at 2180 ft. Alcatraz's road climbs to 733 ft, so at its highest jump the lid is within 150 ft overhead, and the
  earlier clip at w = 4096 (256 ft) cut away the lid above about 25 degrees up: the clear color showed at the top of
  the screen. 16 ft keeps the lid down to a camera nearly touching it; the interpolated frames stay in front because
  both frames' planes are used, and a vertex's w in them differs from the blend of its two w's only by the camera's
  movement times its turn in one frame (a few units).

## No Controller Pak manager

The Controller Pak manager (front-end screen 0xD, `menu_screen_pak_manager_803B5C5C`) lists and deletes the notes on
a real pak. The only pak is the emulated save file, so the screen could only lose saves, and it is never shown, in
any game (`rush2_no_pak_manager`, src/pak.cpp). The front end loop (`menu_frontend_loop_800AE288`) switches screens
at 0x800AE494 when the next screen (0x800E7BB4) differs from the one up (0x800E7BC4, -1 none): a switch to 0xD stays
on the screen that asked for it, or goes to the title screen (0x10) from none or the boot logos (0xE).
