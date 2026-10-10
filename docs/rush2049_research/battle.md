# Rush 2049 battle mode

Port: `src/rush2049/battle.cpp`, hosting in `src/rush2049/track2049.cpp` / `src/rush2049/track2049_menu.cpp`. Tags: **[V]** verified in code or data,
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
| +0x34C | the car's light level, an RGBA gray (white from table `0x8011AE58` at setup, `0x800B1AD8`; recomputed at `0x800940A0`); the car's node and its mounted weapon get it as their color (`func_8008E06C`), alpha replaced while invisible. Not the car's paint |
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

### 6.2 Results, explosion, visibility, pickup details

Main code. **[V]** unless marked.

- **Results** (`func_80105EA8`, the end of every multiplayer race, not a battle's alone): with two or more views a box
  in the middle of the screen (`func_800B3B4C(0x5B, 0x6A - h / 2, 0xE5, 0x72 + h / 2, alpha 0xC0)`) with, in font 0x16
  at (160, 110), text 0x31C `%s WINS` (the winner's name; the winner's car is `0x80143F54`), 0x324 `%d-WAY TIE`
  (`0x80150B60` + 1) or 0x320 `NOBODY WINS` (the winner is a computer car). Each view (positions `0x80115F28` by view
  count) has a box 0x8A wide with, for a battle (game type 6), `"%s\n%d %s"`: the player's name, the kills (car state
  +0x3A3) and text 0x208 `POINTS`. The text table is file 0 (count, offsets, strings); `0x8017A4E0 + 4` points to it.
- **Explosion** (`func_800AF06C(pos, car, size, sound)`; a weapon's is `(pos, 0, 0.5, 1)`): an instance whose model
  steps through the 30 handles at `0x80142908` (`NEXPLOSIONG1` - `G30`, file 61), one every 1/30 s (`func_800908A0`,
  `0x801239C8`), at the size given, with sound 0x2D (size >= 1), 0x45 (>= 0.5) or 0x2F heard within 400. The handle
  table `0x801427C0` is filled from the names at `0x8011AD68` + 4 x (handle - 0x3B) **[I: the offset, from the WEP_*
  handles 0xD8-0xDF]**.
- **Visibility** (`func_8009EBC0`): the battle arenas' tables are `0x8011E428`, `E438`, `E448`, `E458`, **`E468`
  (DM5: 14 regions, the count table `0x8011E748`)**, `E548`, `E558`, `E568`; the other seven have no regions.
- **A pickup's turn**: `func_8010D680` turns it by its object's rate record (object +0x6C, set by the spawner from
  `0x80118D70` + 12 x sub-kind for kind 0): row 0 = (0, 3, 0) rad/s, about its up axis. A taken pickup is hidden
  (`func_8008AE8C`, flag 0x80000000); a weapon's is shown again (`func_8008B0D8`) once no car holds that weapon. Nothing
  in that function shows a power-up again: the 60 s the port uses is **[I]** (the type row's +0x1C is the sound 0x60,
  not a time).

## 7. The port (Rush 2)

- Arenas are hosted in STUNT1 (stunt mode). 2049's battle was multiplayer only; the port's computer opponents are
  section 9.
- `src/rush2049/battle.cpp` reads the mount, muzzle and shield tables from the player's ROM (the overlay is inflated at load).
  Rush 2's own cars (types 0-21) carry roof weapons where 2049's second car does, at their body's height **[I]**.
- **Models** (`src/rush2049/battle_render.cpp`): 2049's files 76 (weapons, projectiles, effects), 63 (coins) and 61 (the
  explosion's frames) are copied as they are into spare RDRAM (0x80E20000 - 0x80E80000) and rebased as 2049's loader
  does, like the wings (src/rush2049/wings_render.cpp). What the battle places (60 slots: 0-31 shots and effects, 32-39 mounts,
  40-43 HUD weapons, 44-51 shields, 52-55 HUD power-ups, 56-59 HUD coins) is drawn at the end of each view
  (`rush2_battle_render_view`, `func_8007C624`'s exit) under a float matrix of its own on the view's root modelview,
  unlit, in a primitive color. So the models need no scene nodes and nothing of the track. The converter's 44 spare
  pool records (how they were drawn before) stay hidden; only the pickups are scene nodes.
- **HUD models are drawn in view space.** The game keeps a view's rotation in the projection matrix (`func_8007C624`:
  perspective, times the look-at; the root modelview is the identity or the mirror), so RT64 takes it for the view
  matrix and a model's matrix for its place in the world, and between two game frames moves the model along a straight
  line while the view turns: a model fixed to the camera slips by about the square of the turn / 4 of its distance
  from the view's middle (7 px measured in the race start's camera, which turns 12 to 20 degrees a frame). The
  renderer loads a camera-attached model's matrix times the look-at as the whole modelview under the perspective
  alone, then puts the view's projection back; `rush2_interp_node_matrix` (src/interpolation.cpp) does the same for a
  scene node registered with `rush2::interpolation_view_attached`. Rush 2's own countdown digits slip the same way and
  are not changed.
- HUD: Rush 2's elements are removed except the speedometer and the clock (callbacks `0x800B7B1C`, `0x800B94C0`: the
  stunt clock counts the round down; elements are in a pool at 0x802F6400, `func_80060418`); the stunt score panels
  are hidden (`func_800B9CC0`, `func_800B7654`). The layout (`rush2::views::view_of`, `layout_of`) gives every element
  a position on the 4:3 screen and an anchor (its view's left edge, middle or right edge), so HUD Placement moves it
  like Rush 2's HUD: one view, two stacked (2049's look: bar 4/3 wider, coins at the line between the views), two side
  by side, quadrants (the coins either side of the clock, which Rush 2 puts in the screen's middle). The bar is a Rush
  2 layout element scaled and anchored by `rush2::hud::set_widget_scale`. Digits are drawn by
  `rush2::hud::draw_number` (its own 8 x 10 font, with high resolution versions in the font pack:
  tools/font_pack/draw_battle_digits.py names them by the RT64 hash of the images in src/hud.cpp).
- HUD models sit 3 units ahead (13 put them under the road); a view's field of view comes from its view struct
  (`rush2::splitscreen::view_tan_v`). The coin model is much larger than the others: its scale is set by eye **[I]**.
- **Colors**: weapons are drawn in their own textures' colors, the same on every car. `func_8008E06C(node, &rgba)`
  sets a node's color (node +0x3C, the primitive color when the node has flag 0x2000, `func_8009C8F0`): the mounted
  weapon gets the car's light level (+0x34C, white), the HUD's weapon white (`0x803942A4`), a muzzle flash `FFFF2B`,
  the held mine's parts `00FF00FF` (`0x80394884`). Pickups are created without the flag. The port draws mounted and
  HUD weapons white. (An earlier version tinted them with the car's paint, misreading +0x34C.)
- **Mounts follow the drawn body**: the mounted weapon and the shield are placed before each view is drawn from the
  car's body scene node (index at `0x80219DD0 + car * 0x134`; its transform at node +4: rotation rows, position at
  +0x24). The draw state's pose is the physics tick's and is a frame behind what is drawn, which made them slide.
- **Weapons cheat health bar**: the same `HEALTHBG` frame and fill as a battle's, at the same place and size. A
  normal track has no `HEALTHBG` image, so it is decoded from 2049's file 63 (4 bit texels, 16 color palette) and
  drawn with `rush2::hud::draw_image`.
- The clock: the stunt clock `0x8010C204` (Rush 2) is 3.5 during the start countdown and 300 afterwards; only the 300
  is replaced by the limit. Rush 2's clock test (func_800AE670 state 3) doesn't end a race without checkpoints, so
  the port sets the out of time flag `0x800FAE98` itself (every tick from the limit on).
- **Results** (section 6.2): from the limit on, `draw_results` (hud_draw) shows 2049's boxes with the game's own text
  (`text_print_string_800734E0`, boxes by `rush2::hud::draw_rect`): `PLAYER n WINS` or `n-WAY TIE` in the middle, and
  in each view `PLAYER n` over `k POINTS`. 2049 prints profile names; the port prints PLAYER 1-4 **[I]**. Nothing is
  fired or scored after the limit.
- **Explosion** (section 6.2): the 30 `NEXPLOSION` frames at half size, 2049's sound 0x45 (which holds until its
  key-off, sent after 1.5 s **[I]**). Wall and car hits of bullets keep a small scaled muzzle flash **[I]**; there are
  no smoke trails, casings or scorch marks.
- **Billboards and muzzles [I]**: each explosion frame is a card in its model's xy plane, seen from the camera along its +z (the other way it is culled), so it is
  drawn facing each view's camera (`battle_render::place(..., billboard)`). A shot starts at the weapon's mount plus
  its muzzle in the car body's drawn pose (`body_pose`), not the physics tick's, which the drawn car is ahead of. The
  tracers (`WFX_TRACERG1` reaches 39 behind its origin, `WPR_CANNG1` 63) are cut so they never reach back past the
  muzzle, and on their first frame stretch from it: a bullet goes 66 a tick, farther than its tracer is long.
- **Invisibility**: `rush2::ghost::set_faded` (src/ghost.cpp, the ghosts' model hook): the car's own view draws the
  whole car translucent, as a ghost; the other views don't draw it (2049 fades it to nothing there), and its shadow
  polygon (car state +0x20C) is hidden in them. The arrow to it is faint (alpha 0x20).
- **Teams** (2049 `0x8012E67C`): Games tab, Player n Battle Team (blue, red, yellow, green; default one each). Cars of
  a team don't damage each other; the coin and the arrows take the team's color.
- **Visibility**: DM5 uses its table (section 6.2); the other arenas draw every section.
- Sounds go through `rush2::audio2049::sfx_start` with 2049's emitter law per local player. The 2049 effects are only
  mixed while something calls `rush2::track2049::effects_running` every frame (they go quiet with the race paused);
  the battle does (`update_sounds`). A pickup plays its type's sound 0x60.
- **Pickups are world-space top-level records after the arena's sections** (the converter, `convert_placement`): a
  pickup's light cone (`ICON_LIGHT`, the last six triangles of a `WEPICON_*L1` model) is translucent and writes no
  depth, so a section drawn after it painted over it wherever the section was behind it.
- **Arrows** (section 6.1, `src/arrows.cpp`): Rush 2 has the same arrows for its two views (`func_80086CA4` makes one
  polygon per player's car, drawn in the other view, in the car's paint; `func_80059B9C` moves it: the same placement
  as 2049's in-view case). One polygon can face one camera only, so with three or four players every view gets a
  polygon of its own for every other player's car, placed as Rush 2 places its one, and Rush 2's are hidden (every
  other polygon with an `ARROW` image); with two players Rush 2's are left alone. In a battle they are 2049's battle
  arrows for any number of players: team colors, primitive depth 1, and the edge arrows, which keep 2049's layout as
  parts of the view (flush with the side, a twentieth of the 4:3 view's width) and are kept whole inside the view
  **[I]** (2049's bottom arrows of two stacked views sit half below it). Polygons are `0x800FAF00` (count
  `0x800FAEF0`, the most used `0x800FAEF4`; `func_80054010` makes one, `func_80053D28` writes one, `func_8007C624`
  draws those with its view's bit); the image is Rush 2's own `ARROW` (texture tables `0x80119220`, count
  `0x800D5788`). The 4 player code draws the polygons of views 0 and 1 in views 2 and 3 too
  (`rush2_players4_poly_mask`); these arrows are left out of that. Shown in a race and its countdown (game state 3
  and 10), as Rush 2 shows its own.
- **The Weapons cheat** (Cheats tab: Off, the eight weapons, Invisibility, Random): in any other race the players'
  cars get the battle's health, weapons, mounts, shots and sounds (`Mode::cheat`; set up at each race's countdown,
  game state 10). A car without a weapon is given the chosen one again 3 s later. Rush 2's HUD stays; a plain health
  bar and the weapon with its ammo are added at the bottom of each view. Computer cars are targets and don't fire.
  Shots are stopped by the collision of a converted Rush 2049 track (`ConvertedTrack::solid_triangles`, now made for
  every 2049 track); on Rush 2's and SF Rush's tracks only grenades and mines meet the ground, taken as level at the
  height of the car that let them go, and other shots fly until they hit a car or their time is up.
- **Track select**: an arena's route band (src/rush2049/track2049_art.cpp, `build_tube`) is a twentieth of the path's extent
  wide instead of 2049's 200 units, open where the path's ends are apart, and rises a little ring by ring: the
  arenas' paths are a tenth of a race track's size and come within a few units of themselves, so 2049's band was a
  blob whose level tops fought for the depth buffer.

## 8. Known differences and test aids

- Computer opponents are the port's own (section 9); 2049's battle was multiplayer only.
- The results print PLAYER 1-4, not profile names; a power-up comes back after 60 s **[I]**; the coin's scale, the
  bullet hit flashes and the non-weapon model colors are by eye **[I]**; an invisible car is a ghost in its own view,
  not a fade.
- Seen in game: 1 and 4 players in DM1 and DM5 (missiles, mines, kills, wrecks, invisibility, the explosion, the
  clock, the results, the arrows), the Weapons cheat in a 1 player practice race, the arrows in a 4 player practice
  race, the track select bands. Not seen: a pickup's light up close, two players stacked and side by side with the
  new renderer, HUD Placement Original and 16:9, teams in play.
- Test aids (env `R2_BATTLE_TEST`, any of): `fire` presses every player's FIRE twice a second; `give<n>` gives every
  car pickup kind n (0-7 weapons, 8 heal, 9 invisibility, 10 shield) at 2 s; `kill` destroys car 0 at 8 s; `short`
  makes the battle 25 s; `log` prints the clock, sounds and pickups; `models` lists the arena's model names.
  4 players by script: `RUSH2_TEST_PLAYERS=4` with P2START / P3START / P4START after the BATTLE row is reached.

## 9. Computer opponents (the port's own)

Rush 2049 had no battle AI, so `src/rush2049/battle_ai.cpp` is new code on top of the port's battle rules. How many
opponents is the BATTLE track select's **DRONES** (0-7; a race has 8 cars, so fewer with more players), and how well
they drive and fight its **DIFFICULTY** slider (0-5, menu settings +0xA, copied to `0x8010C211` by `func_80094698`).
The BATTLE select lists both (`option_open`, `filter_options`), and its DIFFICULTY slider stays on screen: the slider
widget `func_803C5798` moves the value off screen on track 11 (DIFFICULTY at 0x803C5930, HANDICAP at 0x803C5978), and
the hook there (`rush2_track49_stunt_option_t9`) leaves an arena's DIFFICULTY alone.

- **Cars**: the opponents are Rush 2's drones. `func_80094698` copies the track select's DRONES into the race's drone
  count `0x800D3E90`; its stunt test gave the arenas none, and the end of it (`rush2_track49_obstacle_settings`,
  0x80094A30) now writes DRONES (menu settings +0x1, capped at 8 - players) there for a battle arena. The BATTLE
  select keeps DRONES open (`option_open`) and lists it even in stunt mode (game mode 2, which `func_800AE670` sets
  for STUNT1 and the arenas and whose option list `func_803AB294` stops at WIND). The drone slots get their cars and colors as in
  any race (`func_800A37F4`, with the Players tab's AI Opponents choices). A computer car is a drone (car +0x7E8 == 1)
  in a battle arena that is set up (`rush2::battle::active`).
- **Driving**: a drone's driver `drone_driver_80074990` is skipped for them (hook at its start, shared with the
  ghosts) and the AI writes the inputs a player's controls would: car +0x728 steering (-1 left .. 1 right), +0x734
  throttle and +0x730 brake (0-1, as `input_steering_from_stick_80076694` and the code after it write them for a
  player: the gas, stick up or its button, to +0x734 and the brake to +0x730; `car_physics_inputs_80071A1C` copies
  +0x734 to +0x3B4, the drivetrain's throttle), +0x738 gear (1 drive, -1 reverse: `player_join_init_car_80080524` sets -1 while the reverse button is held;
  a race starts at 1, `0x8008D7BC`). The AI runs in the battle's tick (the movers' hook at 0x800765F4, before the cars
  step).
- **Steps**: `physics_tick_drones_80075C3C` steps drones round robin (a few per tick, schedule `0x800CC0E4`), so as
  for the ghosts an opponent's scheduled step is skipped (`physics_car_tick_80075880` start) and each one is stepped
  every physics tick at its end (0x8007660C), with `car_material_effects_update_800663CC` after it when
  `0x800D042C` is set, as the drone loop does. For its step the car is a human's (+0x7E8 = 2, as the ghosts are), so
  the physics treats it as a player's car (a crash wrecks a drone always, a player's car as the cheats allow,
  `car_crash_damage_stage_800715E8`), and its clutch +0x72C is set first by `player_join_init_car_dynamics_800806A4`,
  a player's clutch from rpm and pedals (a drone's stays at the race start's 0). The AI waits for the race (game
  state 3; a drone's release +0x71C is set during the countdown already) and for the car's release after a respawn.
- **Navigation grid**: built on a thread from the arena's solid triangles when it is converted (the ones shots hit).
  Columns of 4 ft (more for a bigger arena, at most 300 across the level surfaces' extent); every surface in a column
  no steeper than 50 degrees (|normal y| >= 0.64) with 3.5 ft of room above it is a node. A node links to the node
  of each of its 8 neighbor columns closest in height that rises at most 1.2 x the step or drops at most 30 ft (a
  drop, one way, costing more) if no triangle steeper than 50 degrees crosses the line between them 2 ft up. Nodes
  missing a link are at a wall; the cost of a node grows within 3 cells of one. Components join nodes linked either
  way. `R2_BATTLE_NAV=<file.pgm>` writes the grid as an image.
- **Routes**: A* (octile distance), avoiding other cars' mines on the ground; replanned every 0.6 s, when the goal
  moves to another node or the car is far off its route. The car steers for the route point 3-10 cells on (by
  speed), or straight at its goal within 60 ft when the way is open and the ground level enough.
- **Skill** by DIFFICULTY: 0 (Easy), 2.5 (Medium) and 5 (Hard) in the table `skills`, the steps between blended;
  dodging from 2 up. Easy / Medium / Hard: decisions every 0.7 / 0.4 / 0.15 s, top speed x 0.78 / 0.9 / 1, aim
  window x 2 / 1.3 / 0.9, 45% / 80% / 100% of their chances to fire taken, pickups looked for within 250 / 400 / 600 ft.
- **Goals** (every 0.15 / 0.4 / 0.7 s by skill, at once when the goal is gone): attack an enemy (nearer, more
  damaged, the last car to hit it, the one it is already after; not with only mines; an invisible car only within
  30 ft), a pickup (a weapon when it has the gun, health by how hurt it is, the power-ups; not one another car is
  twice as close to), or roam to a random open node. A goal that can't be reached is left for 5-10 s.
- **Firing** (the battle's own `fire`, through the same buttons as a player's, `rush2::battle_ai::buttons`): gun and
  gatling when `update_aim`'s yaw is within the target's half width x the skill factor, under 350 ft; cannon and rocket
  when the car points at it (the rocket's flight led), under 400 ft; missile when the target is in a cone ahead
  under 320 ft; grenade within 60 ft plus the car's speed x 0.9; sonic within 55 ft; mine when a car is behind
  within 90 ft. Not through walls (the way 2.5 ft up must be open, except for the lobbed and area weapons). The gatling
  holds FIRE; the others let go a tick between presses. Easy fires at 45% of its chances, Medium 80%.
- **Speed**: the top speed (130 ft/s x 0.78 / 0.9 / 1 by skill) times how little the car must turn (down to a
  quarter). Shooting at a car within 60 ft it holds about 35 ft back at that car's speed once it faces it, and keeps
  going round at 35 ft/s until then (a car turns only while it moves); with the ram it drives into it.
- **Recovery**: a car with the throttle on that hasn't moved for 0.8 s, or (not chasing a car) whose goal is close behind it, reverses
  with opposite lock for about a second. One that hasn't got 15 ft anywhere for 10 s (3 s on its roof) is wrecked
  (car +0x648), and the game respawns it.
- **Dodge** (Medium, Hard): a missile within 140 ft heading at the car makes it swerve across its path for 0.7 s.
- **Results**: the winner can be a computer car (`CPU n`, numbered by drone slot) **[I]**; with one player and
  opponents the player's points box moves below the winner's box.
- Test aids (env `R2_BATTLE_TEST`): `ai` logs each opponent once a second (goal, target, goal point and distance,
  wanted speed, position, speed, inputs, weapon, wreck and respawn state); `bots<n>` races n opponents whatever
  DRONES says; `nofire` keeps them from firing (driving only); with `log` the battle logs every hit
  (`car N takes X from car M (health H)`). `python tools/battle_ai_log.py RUN_LOG` summarizes a run: each car's goals,
  mean speed and time stopped, damage and kills dealt. Recipe (1 player, 3 opponents, round over at ~52 s):
  `R2_BATTLE_TEST=bots3,ai,log,short python tools/rush1/shots.py OUT "8:START,11:DD,11.6:DD,12.2:DD,12.8:DD,13.4:DD,15:A,17.5:A,20:A,22:A,25:A" 40 53`.
- Seen in game (DM1, 1 player, 1 and 3 opponents): they collect weapons and power-ups, chase and shoot each other and
  the player, score kills, and a computer car can win (`CPU 2 WINS`). Every weapon's rule fired and hit, each given
  to all cars with `give<n>` (cannon, grenade, mine, missile, ram, rocket, sonic, and the gun). All cars respawn at the
  arena's one respawn point, so fights gather there. The BATTLE select's DIFFICULTY row and slider were seen and
  moved. Not seen: how DIFFICULTY 0 and 5 play, the other arenas, 2-4 players with opponents, the gatling.
