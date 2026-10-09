# Rush 2049 battle mode

Port: `src/battle.cpp`, hosting in `src/track2049.cpp` / `src/track2049_menu.cpp`. Tags: **[V]** verified in code or data,
**[I]** inferred or approximated. Addresses are Rush 2049's unless a line says Rush 2.

## 1. Where the code is

- The battle logic is **not in the main segment**: it is a mode overlay, raw deflate at **ROM 0xB6FEC4**, 0xAB70 bytes,
  loaded at **0x8038A400** (strings: `STNT_*`, `HEALTHBG/HEALTHBAR`, `WPR_*`, `WFX_*`, `MINE_CAP*`, `HIT_WALL`,
  `SCORCH_MARK`, `X %i`). The setup screen overlay (ROM 0xB5C534) loads at the same address; they are different overlays.
  `python tools/rush2049/decomp.py ovl ADDR` decompiles its functions (m2c; function bounds found by scanning for
  `jr $ra`; jump tables are found from the `lui/addu/lw/jr` pattern and given to m2c as `jtbl_` symbols).
  Function starts: 8038A400 8038A408 8038A8CC 8038A95C 8038AA14 8038AA8C 8038C910 8038CA24 8038CB10 8038CB18 8038CB20
  8038D054 8038D1A8 8038D200 8038D328 8038D3A4 8038D498 8038D798 8038DA78 8038DDDC 8038E088 8038E114 8038F560
  8038F568 8038F938 8038FCD8 8038FCE0 803908D0 80390B00 80390B08 80390B10 80390D38 80390F60 8039133C 80391490
  803914A8 803914B4 80391640 80391648 80391650 80391864 80391B00 8039244C 803925C8 803925D0 80392894 80392FE4
  80393518 803936A8; data from 0x80393B34.
- Game type `0x8014A110 == 6` is battle (stunt 4, obstacle 5). Timer setup at 0x800FC0FC: a battle uses `0x80140BD8`
  minutes x 60 when `0x80140B08 == 1`, else 1200 s. Both come from the options object (+0x25, +0x27); their defaults
  live in the setup overlay and weren't read. The port's limit is the Games tab option (default 3 minutes).

## 2. Per-car state

`0x80152818 + i * 0x3B8` is a car's draw state. The battle code takes a car's pose from it: **+8 position (the model's
origin, at the bottom of the body), +0x14 velocity, +0x2C orientation rows (right, up, forward)**. Rush 2's is
`0x801124A0 + i * 0x354` with the same fields 8 bytes earlier (**+0 position, +0xC velocity, +0x24 rows**, checked in
game) and no battle fields. Battle fields:

| Offset | Meaning |
| --- | --- |
| +0x308 | s8 active |
| +0x34C | color the weapon models are drawn in (alpha replaced while invisible) |
| +0x359 | s8 dead |
| +0x35B | s8 car index |
| +0x380 | pointer to the player's control struct (section 3) |
| +0x384 | s8 weapon: 0 CANN, 1 GATT, 2 GREN, 3 MINE, 4 MISS, 5 RAM, 6 ROCK, 7 SONC, **8 the default gun** |
| +0x385 | s8 ammo (-1 endless: the gun) |
| +0x386 | s16 health, max 0x320 = 800 |
| +0x388 | s16 last damage |
| +0x38C | power-up flags: 1 invisible; 2 shield with phase bits 8 growing, 4 holding, 0x10 shrinking |
| +0x390 | f32 invisibility seconds (30) |
| +0x394.. | shield: +0x394 phase timer, +0x398 scale, +0x39C spin angle |
| +0x3A0 | s8 fired counter (6 on a shot; the mount's muzzle flash reads >= 5) |
| +0x3A1 | u8 alpha while invisible (never under 0x30 in the car's own view) |
| +0x3A4 | s32 mine state: 1 ready, 2 held behind the car |
| +0x3AC | f32 seconds before the weapon fires again |
| +0x3B0, +0x3B4 | f32 aim pitch and yaw of the guns |

`func_8038CA24(car)` resets these at a spawn: weapon 8, ammo -1, no power-ups.

## 3. Controls

A player's control struct is `0x8014A118 + player * 0x4C`: +1 controller port, +4 buttons pressed, +8 held, +0xC
released, +0x10/+0x14 stick, **+0x18 + 4 x n the button mask of function n** (10 functions), +0x48 controller object.
`func_800D7E90` fills the masks from each port's 10-byte configuration (`0x8013C068 + port * 10`, defaults from
`func_800C77C4`: 1, 2, 0x13, 9, 0xF, 0x10, 0xE, 8, 0xD, 7) through the table `0x801118E4` (entry n = `1 << n`).
Function 0 is gas and 1 brake (`func_800E6460`), **9 is FIRE (mask +0x3C, button bit 7)** and **6 is DROP WEAPON (mask
+0x30, button bit 14)**. The button word is the game's own (`0x80156CF0 + port * 0x10 + 4`): bit 1 = A, bit 2 = B, bits
10-13 the D-pad (up, down, left, right; the stick sets them in menus). Which N64 buttons bits 7 and 14 are wasn't traced
to the pad driver **[I: bit 7 is taken to be Z]**.

The port has its own bindable actions instead (FIRE, DROP WEAPON; `rush2::controls::Action`, two more rows of Rush 2's
Controller Setup screen), since Rush 2's layout already uses every button.

## 4. Weapons

`func_8038FCE0` runs once a step for every car: power-up timers (`func_8038F938`), aim (`func_8038F568`, weapons 0, 1,
6, 8), the DROP button (weapon = 8, ammo = -1), the cooldown, and firing. A car fires when the cooldown is 0, ammo isn't
0, it isn't wrecked, and **FIRE was pressed (or is held with the gatling)**. The ram doesn't fire. With the mine the car
first holds one behind it (state +0x3A4 = 1 then 2) and FIRE lets it go. `func_8038E114` then moves every shot.

Per weapon **[V]** (`D_8002EB94` is the step's seconds; "explodes" means `func_8038D798` with radius^2 900):

| Weapon | Cooldown | Life | Radius | Motion | Damage |
| --- | --- | --- | --- | --- | --- |
| 0 cannon | 1/6 s | 4 s | 0.5 | 1000 ft/s along its aim (pitch only) | 400, direct |
| 1 gatling | 1/15 s | 4 s | 0.25 | 2000 ft/s, aim plus up to +-0.0125 rad spread on both axes; ejects a casing | 114, direct |
| 2 grenade | 1/6 s | 3.1667 s | 0.5 | launched 0.5 rad up at 75 ft/s plus the car's velocity; gravity x 2.5; bounces off the arena keeping 0.9 | 800, explodes |
| 3 mine | 1 s | 5 s in flight | - | let go with 15 ft/s up plus the car's velocity; gravity; horizontal speed x 0.9 a step; first bounce leaves at 15, later ones keep 0.4; at rest it becomes a `WPR_MINE` object | 800 to the car that touches it |
| 4 missile | 1 s | 15 s | 2.0 | speed = the car's forward speed + 165, then toward 100 ft/s; homes (below) | 960, explodes |
| 5 ram | - | - | - | passive (`func_800CE358`, main): damages a car it hits, about 160 + 7 x speed, using an ammo | |
| 6 rocket | 1/6 s | 10 s | 1.0 | 50 ft/s along its aim (pitch only) plus the car's velocity, then accelerates 800 ft/s^2 | 520, explodes |
| 7 sonic | 1 s | 1 s | - | a ring at the car (below) | 800 / 400 / 200 |
| 8 gun | 1/6 s | 4 s | 0.5 | 2000 ft/s along its aim (yaw and pitch) | 80, direct |

Ammo from a pickup, `0x80121D60`: 20, 100, 20, 3, 3, 5, 20, 5; the same weapon again adds it. Sounds, `0x803942C0` by
weapon: 0x5C, 0x57, 0x5A, 0x58, 0x5B, 0x5D, 0x5E, 0x5F, 0x59 (`func_800B61A8(sound, car, 1, 1)`).

- **Where a shot starts**: the car's position + (`0x803943A4[weapon][car model]` + `0x80394B08[weapon]`) in the car's
  frame (x right, y up, z forward). The first table (8 weapons x 13 car models x vec3) is also where the weapon's model
  sits on the car (`func_8038AA8C`, which also swaps the model: ids 0xD8-0xDF of table `0x801427C0`, hidden for the gun).
  The gun uses the second table alone, (0, 1.5, 2.0).
- **Aim** (`func_8038F568`): the nearest car ahead within 2000 with |x| <= z / 2 and |y| <= z in the car's frame; the aim
  yaw and pitch move 0.01 rad a step toward `atan2(x, z)` and `atan2(y, z)` (toward 0 with no target).
- **Homing** (`func_8038DDDC`): the nearest car within 2000 inside |x| <= z, |y| <= z of the missile's frame; the missile
  turns 0.04 rad a step in yaw and 0.03 in pitch toward it.
- **Hitting a car** (`func_8038DA78`): the step's segment against every other live car, as a cylinder: the closest
  point in x/z must be within radius + 6.25 and between -radius and 3.5 + radius above the car's position. A car
  holding the ram hit within 1.35 rad of its nose takes 0.35 of a direct hit.
- **The arena** (`func_800ADD58(from, to, out, radius, 0, 0xFFFF)`, main): explosive shots explode, grenades and mines
  bounce. The port sweeps the segment against the converted collision's triangles
  (`ConvertedTrack::solid_triangles`) and stops bullets there (2049 leaves a bullet at the wall until its life ends).
- **Damage** (`func_8038D3A4(attacker, victim, amount)`): nothing to the attacker itself or its team
  (`0x8012E67C` per player); x **0.2** while the victim's shield is on (+0x38C & 4); at 0 health `func_800C55E4` (the
  kill) and the car's wrecked flag.
- **Explosion** (`func_8038D798(pos, from, owner, 900.0, damage)`): every live car within 30 takes
  `((900 - d^2) / 900)^2 x damage`; a ram facing the shot's path within 1.35 rad takes 0.35 of that.
- **Sonic** (`func_8038D498`): the ring's size grows by 1 every 1/30 s for its 1 s; it reaches 4 x its size. Each car is
  hit once: 800 while the size is <= 8, 400 while <= 20, then 200, with a force away from the center of 330000 (+66000
  up), x 1, 0.6, 0.5.
- **Placed mine** (`WPR_MINE`, type row 120: radius 12.5, 45 s; `func_8010C7F4` hit, `func_8010C974` update): a car
  within the radius (+3.5) takes 800, a ram that meets it nose first 280; a car has at most 3 (the oldest goes).

## 5. Pickups and power-ups

Type table rows 102-113 (`WEPICON_*`, `0x80117530 + row * 0x30`): hit `func_8010D3C0`, update `func_8010D680`, radius 6
(POWUP 4), +0x1C 60.0. Pickup anim ids 0x15E-0x168 select what a pickup gives: CANN, GATT, GREN, HEAL, INVS, MINE,
MISS, RAM, ROCK, SHLD, SONC; POWUP is a random power-up. Each arena places the 8 weapons and 6-9 POWUPs.

- A pickup turns in place by its object's own rate **[I: 3 rad/s in the port]**.
- A taken **weapon pickup comes back when no car holds that weapon** (`func_8010D680`); a power-up after its 60 s **[I]**.
- HEAL sets the health to 800. INVS: flag 1 for 30 s (the car and its weapon fade). SHLD: the globe `WFX_SHIELDG1`
  grows 0.05 a step to the car model's size (`0x80394358`), holds 30 s, shrinks; damage x 0.2 meanwhile.

## 6. HUD

`func_80391B00` (with `func_80391864` and `func_80391650`), per view:

- **Health bar**: `HEALTHBG` (64 x 8 frame) with `HEALTHBAR` (64 x 4, tinted) in it, 2D; positions `0x80394150`
  (centers): 1 view (160, 208); 2 views (120, 98), (120, 208); 3-4 views (80, 98), (240, 98), (80, 208), (240, 208).
- **Weapon held**: its `WEP_*` model 2 units ahead of the camera at a screen position (`func_800A6094(view, xy, camera,
  2.0, out)`), scale 0.069 (sonic: 0.175 at 5.75), turning; shown in its view only (`func_8008B0D8(model, 0, 1 <<
  view)`). Positions `0x803941D0`: (32, 208); (32, 98), (32, 208); (40, 98), (280, 98), (40, 208), (280, 208).
- **Power-up in effect**: `WEPICON_INVSG1` / `WEPICON_SHLDG1`, scale 0.025, turning; positions `0x80394210`:
  (256, 218); (176, 108), (176, 218); (120, 108), (200, 108), (120, 218), (200, 218).
- **Coin** (kills): `BCOIN_BLUE/RED/YELLOW/GREENG1` by player, scale 0.012 (0.009 with 3-4 views), tipped a quarter turn
  to face the camera; positions `0x803940D0`: (280, 12); (224, 98), (224, 122); (144, 98), (176, 98), (144, 122),
  (176, 122). The count is printed on it.
- With two views the 2D coordinates are in a 240-wide space: on screen the bar is 4/3 wider and everything sits 4/3
  further from the left edge than the numbers say (measured from a capture of the game). `WEAPONHUD` (128 x 16) and
  `AMMOCOUNT` aren't used by this overlay.
- Arrows to the other cars: section 6.1.

### 6.1 Arrows to the other cars

Main code, not the overlay. **[V]** unless marked.

- `func_800B1B48` (race setup) gives every car a shadow polygon (`SHADBLUR`) and, with two or more players, one polygon
  per other view with the **`ARROW`** image (16 x 16, effects file 62; handle `0x80161376`, loaded from the name table
  `0x8011B404` by `func_800B2B80`'s loop): `func_800A78BC(4, template 0x8011AD90, texture, color, flags, 1)` with flags
  `(1 << view) | 0x3200` in a battle (`0x1200` otherwise), then polygon +6 = 1. The record (car state +0x218, 0x18 each)
  holds +4 the view, +6 the polygon's index, +8 the car, +0x14 the callback `func_8008C884`.
- Polygons are `0x8015B268 + i * 0x58`, the same system as Rush 2's `0x800FAF00` (below): +0 s16 vertex count (0 = free),
  +2 u16 flags, +4 texture handle, +6 s16 primitive depth, then 0x14 per vertex (f32 x, y, z times 16; s16 s, t; RGBA).
  `func_8008C074(poly, count, vertices, texture, color, flags, set_texture)` writes one. Flags: bits 0-3 the views it is
  drawn in, 0x8000 hidden, 0x2000 drawn at the primitive depth of +6 (1: in front of everything), 0x4000 a 2D rectangle.
- Colors `0x8011B558` (RGBA): blue `0000E0`, red `E00000`, yellow `E0E000`, green `00E000` by player (by team
  `0x8012E67C` in a battle), `E0E0E0` for a car without a player.
- `func_8008C884(record)`, every frame. The arrow is hidden while the HUD is off (`0x801613A8`, `0x801174B4 & 0x400000`)
  and, in a battle, for a dead or wrecked car; an invisible car's arrow takes the car's fade as alpha, never under 0x20.
  With d = the car's position less the camera's, in the camera's frame (`func_8008C544`), and
  a = `func_8008C768(-d.x, d.z)` (atan2):
  - **Not a battle, or |a| < 0.48 x the view's horizontal field of view** (the car is in view): the template's corners
    (1, 1), (-1, 1), (-1, -1), (1, -1), turned to face the camera, times s = |d| / 25, around the car's position
    + (0, s + 5, 0). The image's point is down.
  - **Battle, 0.48 x fov < |a| < pi / 2**: at the view's left or right edge. In view space: z = 10 pi / fov (20 with the
    game's quarter turn), x = +-fov x (2 / pi) x 19, y = -vfov x (2 / pi) x 16.5 x (|a| - 0.75 vfov) / (pi / 2 - 0.75 vfov)
    (one view: 14.5 and 1.1 vfov, which no battle uses); the corners +-1 around that, assigned one or three vertices
    round so the image points out.
  - **Battle, behind**: with b = a - pi or a + pi, x = fov x (2 / pi)^2 x 19 x b, y = -vfov x (2 / pi) x 16.5: along the
    bottom edge, pointing down.
  The field of view is `0x80154188` = 90 degrees (`func_800A5744` fills the view struct `0x8017A510 + view * 0x48`: +0xC
  and +0x10 the angles in radians, +0x14 / +0x18 their half tangents, +0x24.. the view's size and center).

## 7. The port (Rush 2)

- Arenas are hosted in STUNT1 (stunt mode), **no computer cars** (2049's battle was multiplayer only; AI is future work).
- `src/battle.cpp` reads the mount, muzzle and shield tables from the player's ROM (the overlay is inflated at load).
  Rush 2's own cars (types 0-21) carry roof weapons where 2049's second car does, at their body's height **[I]**.
- Weapon models, shots, shields and the HUD's models are pool records in the converted placement (44, `WPR_MISSG1`
  items hidden at the origin): 0-15 shots and effects, 16-23 mounts, 24-27 HUD weapons, 28-35 shields, 36-39 HUD
  power-ups, 40-43 HUD coins. The HUD's models are posed per view in `rush2_battle_view` (Rush 2 `func_8007C27C` entry,
  which runs before each view is drawn: $a1 = that view's camera position).
- HUD: Rush 2's elements are removed except the speedometer (elements are in a pool at 0x802F6400, `func_80060418`);
  the stunt score panels are hidden (`func_800B9CC0`, `func_800B7654`). The layout (`view_of`, `layout_of`) gives every
  element a position on the 4:3 screen and an anchor (its view's left edge, middle or right edge), so HUD Placement
  (Original, 16:9, window edges) moves it like Rush 2's HUD: one view, two stacked (2049's look: bar 4/3 wider, coins
  at the line between the views), two side by side, quadrants. The bar is a Rush 2 layout element scaled and anchored
  by `rush2::hud::set_widget_scale` (src/hud.cpp rewrites its texture rectangles in the 2D display list, as it halves
  the deaths skull; texture steps are signed, HEALTHBG is drawn flipped). Digits are drawn by
  `rush2::hud::draw_number` (its own 8 x 10 font; the game's text is queued and drawn later, so it can't be anchored).
- HUD models: a view's vertical field of view has tangent 0.75 (full height) or 0.375 (half height, **[I]** for
  quadrants), the horizontal one follows the view's drawn shape (to the window's edge in widescreen); they sit 3 units
  ahead (13 put them under the road). The coin model is much larger than the others: its scale is set by eye **[I]**.
- Rush 2's nodes carry no color: the weapon models are drawn in the primitive color left by the 2D drawing, which the
  battle HUD leaves at a fixed steel blue (2049 uses each car's color).
- The clock: the stunt clock `0x8010C204` (Rush 2) is 3.5 during the start countdown and 300 afterwards; only the 300
  is replaced by the limit.
- Sounds go through `rush2::audio2049::sfx_start` with 2049's emitter law per local player. The 2049 effects are only
  mixed while something calls `rush2::track2049::effects_running` every frame (they go quiet with the race paused);
  the battle does (`update_sounds`), or its sounds are silent unless a 2049 car's engine is running. The explosion's
  sound 0x45 holds until its key-off, sent after 1.5 s **[I]**. A pickup plays its type's sound 0x60.
- **Pickups are world-space top-level records after the arena's sections** (the converter, `convert_placement`): a
  pickup's light cone (`ICON_LIGHT`, the last six triangles of a `WEPICON_*L1` model) is translucent and writes no
  depth, so a section drawn after it painted over it wherever the section was behind it.
- **Arrows** (section 6.1): Rush 2 polygons (`0x800FAF00`, count `0x800FAEF0`, the most used `0x800FAEF4`; `func_80054010` makes one,
  `func_80053D28` writes one, `func_8007C624` draws those with its view's bit), made and moved by `update_arrows` for
  the view about to be drawn (`rush2_battle_view`), in 2049's player colors, at primitive depth 1. The image is Rush 2's
  own `ARROW` (its effects container; found by name in the texture tables `0x80119220`, count `0x800D5788`). Rush 2
  has arrows of its own over the first two players' cars, without the edge arrows; a battle hides them (every other
  polygon with an `ARROW` image). The edge arrows keep 2049's layout as parts of the view (flush with the side, a
  twentieth of the 4:3 view's width) so they follow widescreen and the side by side layout; they are kept whole inside
  the view **[I]** (2049's bottom arrows of two stacked views sit half below it). The 4 player code draws the polygons
  of views 0 and 1 in views 2 and 3 too (`rush2_players4_poly_mask`); the arrows are left out of that. Shown in a race
  and its countdown (game state 3 and 10), as Rush 2 shows its own **[I]**.
- **HUD models are drawn in view space** (`rush2_interp_node_matrix`, src/interpolation.cpp). The game keeps a view's
  rotation in the projection matrix (`func_8007C624`: perspective, times the look-at; the root modelview is the
  identity or the mirror), so RT64 takes it for the view matrix and each node's matrix for its place in the world, and
  between two game frames moves the node along a straight line while the view turns: a node fixed to the camera slips
  by about the square of the turn / 4 of its distance from the view's middle (7 px measured in the race start's
  camera, which turns 12 to 20 degrees a frame). For a node registered with `rush2::interpolation_view_attached` the
  node's matrix times the look-at is loaded as the modelview under the perspective alone, and the node's pop puts the
  view's projection back. Rush 2's own countdown digits slip the same way and are not changed.
- The battle's digits have high resolution versions in the font pack (tools/font_pack/draw_battle_digits.py, which
  names them by the RT64 hash of the images in src/hud.cpp).

## 8. Not done / known issues

- **Explosions**: 2049 calls main `func_800AF06C(pos, 0, 0.5, 1)` after `func_8038D798`: the game's own explosion
  object (model handle `0x80142908`, callback `func_800908A0`) at scale 0.5, with sound 0x45 within 400 (the function
  picks 0x2D at scale >= 1, 0x45 at >= 0.5, else 0x2F). `func_8038D798` applies no force. The port plays 0x45 and
  shows a scaled muzzle flash (`WFX_MFLSHG11`), not that object. Rush 2's wreck fire is `particle_emitter_spawn`
  (0x8008270C, kind 4), which follows a car and can't be placed freely. No smoke trails, casings or scorch marks.
- **Invisibility**: the port hides the car body's scene node (Rush 2 car state +0xF0 = node index) in the other
  players' views, per view (`rush2_battle_view`); wheels and shadow stay. 2049 fades the car. Car state +0xE4 bit 8 is
  only read by the car select preview, it does not hide a car in a race.
- **Colors**: weapon models are drawn in the player's coin color (`rush2::interpolation_node_color` sets a node's
  primitive color with its matrix); 2049 uses the car's color.
- **Time limit**: Rush 2's clock test (func_800AE670 state 3) doesn't end a race without checkpoints (and never with
  the No Checkpoints cheat); the port sets the out of time flag `0x800FAE98` itself at the limit and the game ends the
  race. There is no winner screen.
- Arrows: the fade of an invisible car's arrow is a fixed alpha 0x20; teams aren't in. Not run: two players (stacked
  and side by side), a car behind in a moving race.
- The pickups' lights after the change of their draw order were not looked at in game (their records and nodes were
  checked: 16 world-space records after the sections, at 2049's positions).
- HUD: view field of view comes from the view struct (`rush2::splitscreen::view_tan_v`; quadrants keep 0.75); HUD
  models are registered with the frame interpolation as camera attached (a fast camera otherwise counted as a
  teleport and left them behind); the battle HUD draws only in a race (game state >= 3) and not over the pause menu.
- Seen in game (3 players): missile hits, kills on the coins, wrecks and respawns, invisibility, the time limit, the
  quadrant HUD, Controller Setup's FIRE / DROP rows (shown only from a battle's pause menu). Not run: pickups by
  driving, mines, sonic, ram, HUD Placement Original / 16:9.
- DM5's PVS table (the arenas draw every section); team play (2049's `0x8012E67C`).
- Test aids (env `R2_BATTLE_TEST`, any of): `fire` presses every player's FIRE twice a second; `give<n>` gives every
  car pickup kind n (0-7 weapons, 8 heal, 9 invisibility, 10 shield) at 2 s; `kill` destroys car 0 at 8 s; `short`
  makes the battle 25 s; `log` prints the clock; `models` lists the arena's model names. 3 players by script:
  `RUSH2_TEST_PLAYERS=4` with P2START / P3START after the BATTLE row is reached (see the memory recipe).
