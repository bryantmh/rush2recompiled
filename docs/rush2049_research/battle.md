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
  minutes x 60 when `0x80140B08 == 1`, else 1200 s. Both come from the battle options (+0x25, +0x27; +0x26, the points
  to win, goes to `0x80142510`), copied by `func_800C9BE0` / `func_800DE45C`. The setup overlay (ROM 0xB5C534,
  record 0x80146108, left / right at 0x8038EC80-0x8038ED70) edits them: +0x25 time limit on / off, +0x26 points 5-50
  in steps of 5, +0x27 minutes 1-20 (wrapping); its reset (step 0) gives off, 10 points and 8 minutes, so 2049's
  default battle is first to 10 points with a 20 minute clock. The main loop at 0x800FC608 compares each player's
  points (0x80152570) with the target: one short plays a sound (`func_800B61A8(2, 0, 2, 0)`), reaching it ends the
  round (`func_800F7F3C`). The port has the same rules (section 7.1).

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
| 5 ram | 1/3 s | - | - | passive, in the car against car collision response (`func_800CE358`, main; below) | 160 + 7 x speed |
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
- **Ram** (`func_800CE358`, 2049's car against car collision response; Rush 2 `car_car_collision_response_8006E2A8`):
  in a battle (type 6) between cars of different teams, a car holding the ram (+0x384 == 5) whose cooldown (+0x3AC)
  is done hits the other car if that car (not being put back, +0x6C4 == -1) is ahead of its middle by more than 2.25
  ft in its frame (snapshot position +0x794 through matrix +0x7A0): `func_8038D3A4(rammer, hit, 160 + 7 x the rammer's
  speed +0x3F0)` (ft/s), ammo -1, cooldown 1/3 s (`0x80124108`); else the other car's ram the same way. Taking the
  ram makes the car 3 ft longer in front (`func_8010D3C0`: box corners +0xFC and +0x108 = the car table
  `0x8011F844`'s length ahead + 3, radius +0x654 = sqrt(max(ahead, behind)^2 + half width^2 + height^2)); when its
  ammo runs out `func_8038FCE0` puts the car's own size back (DROP doesn't). Table `0x8011F844`, 16 bytes per 2049
  car: length ahead, behind, half width, height (car 0: 6.75, 5.25, 3, 3.5).
- **Placed mine** (`WPR_MINE`, type row 120: radius 12.5, 45 s; `func_8010C7F4` hit, `func_8010C974` update): a car
  within the radius (+3.5) takes 800, a ram that meets it nose first 280; a car has at most 3 (the oldest goes).

## 5. Pickups and power-ups

Type table rows 102-113 (`WEPICON_*`, `0x80117530 + row * 0x30`): hit `func_8010D3C0`, update `func_8010D680`, radius 6
(POWUP 4), +0x1C the sound 0x60, +0x14 the animation id. Pickup anim ids 0x15E-0x168 select what a pickup gives:
CANN, GATT, GREN, HEAL, INVS, MINE, MISS, RAM, ROCK, SHLD, SONC. Each arena places the 8 weapons and POWUPs (none
places HEAL, INVS or SHLD themselves). **The POWUPs are spots for the health, shield and invisibility power-ups**,
which the battle overlay's three managers place there in turn (below); row 113's own animation, 0x162, and model
`WEPICON_INVSG1` are only what a spot starts with.

- A pickup turns in place at its object's rate, 3 rad/s (below).
- A taken **weapon pickup comes back when no car holds that weapon** (`func_8010D680`); power-ups come and go by the
  managers.
- **Power-up managers** (overlay; table `0x80399AE0`: +0 the spot count, +1 / +2 / +3 the health / shield /
  invisibility manager's spot, +4 / +0xC / +8 their timers, +0x10 the spots, 8 bytes each: object, s16 +4 (3 = the
  spot last taken from), s16 +6 (1 free, 3 holding a power-up)):
  - `func_8039133C` at the start (after `func_80391490` counted the POWUPs, which `func_8010D3C0` hides as the battle
    is set up, flag 0x20): a spot per POWUP; timers health 5 + random(10) s, invisibility and shield 15 + random(45)
    s each, and 45 s more for one of the two by a coin (`func_8008B2E4(x)` = random in [0, x)).
  - Each step `func_80390F60` runs the health manager `func_80390D38`, the shield's `func_80390B10`, then (after the
    cars' invisibility fades) the invisibility's own. A manager whose timer runs out picks a random spot and from it
    the first one free and not the last taken from, marks it held, and puts its power-up there: the object's
    animation +0x50 = 0x161 (health), 0x167 (shield) or 0x162 (invisibility), so `func_8010D3C0` gives that, its
    model swapped (`func_80090770`) and shown (`func_8008B0D8`); its timer is then -2 (one is out).
  - Once a car takes it (the object's +4 & 2 cleared), the next is due in 5 + random(10) s (health) or 90 +
    random(60) s (shield, invisibility); that spot becomes the one last taken from and is free again.
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
  in that function shows a power-up again: the managers in section 5 do (the port once brought one back after 60 s,
  a misreading of the type row's +0x1C, which is the sound 0x60).

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
  is replaced by the limit (1200 s without a time limit, section 7.1). Rush 2's clock test (func_800AE670 state 3) doesn't end a race without checkpoints, so
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

### 7.1 Battle rules (the BATTLE select's POINTS and TIME LIMIT rows)

2049's rules (section 1): the points to win, 5-50 in steps of 5 (default 10), and a time limit, off by default or 1-20
minutes. 2049 keeps on / off and the minutes apart (its minutes default to 8); here they are one row, OFF then 1-20
MIN. A round ends when a car reaches the points (2049's main loop at 0x800FC608, which plays sound 2 the first time a
car is a point short) or when the clock runs out: the time limit, or 2049's 1200 s without one. Both rows wrap as
2049's do. They replaced the Games tab's Battle Time Limit, which is hidden and only read once: a save without
`"minutes"` in its `track_select` section takes that option's minutes if they were changed from its old default (3),
else no time limit. The section keeps `"points"` and `"minutes"` (0 off).

- **The rows** are the track select's LAPS (option 5) and CHECKPOINTS (option 9) rows, which a battle doesn't use.
  The BATTLE select lists TRACK, FOG, WIND, POINTS, TIME LIMIT, DRONES, DIFFICULTY (`filter_options`), kept open by
  `option_open` (src/rush2049/track2049_menu.cpp). The option labels are `0x800C48F8[language * 11 + option]`:
  0 TRACK, 1 BACKWARD, 2 MIRROR, 3 FOG, 4 WIND, 5 LAPS, 6 DRONES, 7 DIFFICULTY, 8 HANDICAP, 9 CHECKPOINTS, 10 DEATHS.
- Hooks: `rush2_track49_battle_rule_label` (`options_text_cb_names_values_803C6268` at 0x803C6414, $s0 the label
  about to be placed, $s1 the row's list entry) and `rush2_track49_battle_rule_arrows`
  (`options_widget_cb_option_arrow_803C59A0` at 0x803C5C18, $a3 the cursor row's label about to be measured for the
  left arrow) put POINTS and TIME LIMIT in; `rush2_track49_battle_rule_value` (0x803C666C, LAPS' case: digits 1-8
  from x 0xFF; 0x803C6770, CHECKPOINTS' case: OFF / ON; y in $s2) prints the value centered on x 0x112 and skips to
  0x803C6908 (the next row); `rush2_track49_battle_rule_step` (`trackselect_frame_803ABE0C` at 0x803AC278, LAPS'
  left / right case, and 0x803AC3A4, CHECKPOINTS'; $a1 the step, -1 / 1, or 0 for the default) steps the rule and
  skips to 0x803AC410. The strings are at 0x80300F40 (POINTS), 0x80300F70 (TIME LIMIT) and 0x80300F50 (the value).
- **The clock**: the time left (`hud_widget_cb_countdown_time_800B94C0`) shows with a time limit and is hidden
  without one (1200 s is only the cap, and Rush 2's time left stops at 999).
- Each car counts its own kills, teams too; the computer opponents' kills count as a player's.
- Seen in game: the rows, their values and wraps on the BATTLE select; a 5 point round with three computer
  opponents (no clock; the sound when car 1 was a point short; CPU 1 WINS and the points boxes when it had them);
  the clock from 60 with a 1 minute limit. Test aid `winkills` makes car 1 a point short at 8 s and gives it the
  points at 10 s.

### 7.2 Battle in races (the race select's BATTLE row)

Not in 2049. The race track select (ONE RACE, CIRCUIT, PRACTICE; not GHOST RACE, STUNT or BATTLE) has a BATTLE row
below DEATHS, OFF by default, kept in the save's `track_select` section as `"race_battle"` (a save without it is off).
It needs the Rush 2049 ROM (the battle's models) and is read when a race starts (`Mode::race` in
src/rush2049/battle.cpp; not in stunt mode, `0x8010C3E8 == 2`, or a ghost race).

- **The row** is option 11, past the game's 0-10, so every place that indexes a table by the option is hooked
  (src/rush2049/track2049_menu.cpp, `rush2_track49_battle_*`):
  - label: `options_text_cb_names_values_803C6268` at 0x803C6410 (about to load `0x800C48F8[language * 11 + option]`
    into $s0, which an 11 reads past) sets $s0 to "BATTLE" (0x80300F60) and skips the load to 0x803C6414;
    `options_widget_cb_option_arrow_803C59A0` at 0x803C5C14 does the same for the arrows' label width ($a3, to
    0x803C5C18).
  - value: the value jump table 0x803CAF38 has cases for options 1-10, and 0x803C644C skips an 11 to the next row
    (0x803C6908), so 0x803C6440 (after the label is printed, $s2 the row's y) draws it first, as BACKWARD's case 1
    (0x803C6468) does: the language's two words `0x800C4A90[language * 2 + 0/1]` (off, on) at x 0x100 and centered on
    x 0x124 (`text_center_x_variant_8007347C`), the chosen one in style 0xA (green) and the other in style 4.
  - left / right: the jump table 0x803CAC10 has options 0-10 and sends an 11 to 0x803AC410 (which copies the menu
    settings to the race's), so 0x803AC078 ($t9 the option, $a1 the step: -1 / 1, or 0 for the default) toggles it
    (the default is off) and saves.
  - With HANDICAP (2 players or more) the list is 12 long; the twelfth entry is 0x803D0604, which is the car select's
    (`menu_select_player_set_state_803B2BB4` sets it when that screen opens). The track select's screen and list are
    set up again each time it opens.
- **The race**: every car has the battle's health (800) but no default gun (the gun, weapon 8, doesn't fire in
  `Mode::race`: only the pickups' weapons do, and a car is back to nothing when one runs out or is dropped); the
  weapons and power-ups work as in an arena (the
  Weapons cheat's flat-ground shots on tracks without the battle's collision), and a car brought to 0 is wrecked and
  respawned by Rush 2 as any wreck. Nothing is fired or taken before GO.
- **Time**: half as much again. `rush2_track49_race_time` (`path_load_derive_80093048` at 0x80093298, after the game
  worked out the start time, path header copy +0 at 0x8010BCE8, and each checkpoint's extensions +0x1E / +0x20 from
  the AI lanes; Rush 1 and 2049 tracks set theirs there too) multiplies them by 1.5 when
  `rush2::battle::race_battle_applies` (the row on, the Rush 2049 tracks there, not stunt mode, a battle arena or a
  ghost race). Seen: the test track's start time 38 -> 57 s with the row on, unchanged with it off.
- **Missile** (3 per pickup, as 2049): faster than the arenas' so it can catch a car at full speed. It starts at the
  car's speed + 165 ft/s and settles at 360 ft/s (245 mph; 2049's settles at 100) (its gap shrinking by 2x its size per second,
  about halved every 0.35 s), and turns speed / 100 times 2049's rate (the same turning circle), each step stopping at the target's
  direction. Seen in game: missiles at 360 ft/s, one closing on a drone from 118 ft and hitting it
  (`R2_BATTLE_TEST=log,give4,missile` logs each missile's speed and target distance).
- **Pickups**: a row on each checkpoint line but the start / finish line (header +4, 0x8010BCEC), where the
  grid waits (Rush 2's path, docs/rush2049_research/race.md section 3: the header copy 0x8010BCE8 has the checkpoint
  count at +8 (0x8010BCF0) and the checkpoints from +0xC (0x8010BCF4, 0x50 each: +0 the gate's middle, +0xC the
  direction of travel, +0x22 where the spine, lanes 1-4 and branches cross it); the lane headers at 0x80110020, 8 each,
  +0 point count, +4 points of 8 bytes). As many as fit 12 ft apart (1 to 8) are evenly spaced (one in the middle of each equal part) across the road
  at the line, which is measured with Rush 2's ground query `collision_ground_query_8006CF00(pos, out_point,
  out_matrix)` (it puts cars on the grid and back on the track; returns the POLY record of the polygon nearest in
  height within 250 ft, walls skipped, or 0; +0 u16 flags, & 0xF the surface type). From the gate's middle it
  probes 1 ft steps to each side, up to 60 ft, while there is ground of type 0-2 with no step over 0.45 ft (a curb;
  a banked road rises about 0.35 ft a foot), except a step of up to 1 ft within 15 ft of the middle (a median the
  middle is on). The row stays 5 ft in from those edges, at least 20 ft wide, 2.5 ft over the ground under each
  pickup. Game code is called with `battle::tick`'s caller's context (the movers hook), its stack borrowed below
  its stack pointer; without it the AI lanes' crossings give the width. Each is a random one of the 8 weapons,
  health, invisibility or shield (the `WEPICON_*` models of file 76, drawn by src/rush2049/battle_render.cpp in
  slots 60-159; a line that doesn't fit is left empty). One taken comes back 7 s later (about Mario Kart's item boxes) in its place as another kind. On the test track the
  road came out 68-120 ft wide (the material is not a test: a gate's middle can be on a patch of another one).
  Test aids: `R2_BATTLE_TEST=road` logs where each side stopped, `startrow` puts a row on the start line too (in
  view on the grid; the race starts about 23 s into the recipe below).
- **HUD**: the Weapons cheat's health bar, with the weapon held centered just over it and its ammo to its right, and
  the power-up in effect over the bar's right end (beside the bar they ran into Rush 2's time, place and radar in
  quadrants). These models turn about their own middle: `rush2::battle_render::model_center` takes the middle of the
  bounding box of the vertices a model's display list loads (G_VTX, 1/16 ft), and `center_model` moves the model's
  origin so that middle is at the HUD point (their origins are off to one side, so they swung around it). The arenas'
  HUD is as before.
- **Computer cars** keep Rush 2's drone driver. `rush2_battle_race_steer` (`drone_driver_80074990` at 0x800751C8,
  just after it stored the steering $f0 to car +0x728, $v1 the car; -1 .. 1, right positive) moves the steering by
  at most 0.35 toward a wanted pickup 15-170 ft ahead and no more than 12 ft + a tenth of the distance to the side (a
  weapon while it has only the gun or the same one, health below 3/4, the other power-ups always).
  `rush2::battle_ai::race_buttons` fires with the arena's rules (`should_fire`, no line of sight test) at the race's
  DIFFICULTY, from 4 s after GO (the pack is close at the start), only with a pickup's weapon.
- Seen in game (Marina, 1 and 4 players, 2026-10-09): the row and both values; first 18 pickups on 6 lines, then 25 on Marina's 5 lines past the start; drones taking
  pickups on every line and steering into them (`R2_BATTLE_TEST=steer` logs it); the weapon, ammo and invisibility
  HUD in 1 player and quadrants. Test: set `"race_battle": 1` in build/saves' `track_select` (or toggle the row) and
  `R2_BATTLE_TEST=log,give4 python tools/rush1/shots.py OUT "8:START,11:A,13.5:A,17:A,19.5:A,22:A,24.5:A,27:A,31:A:70" 33 45`
  (the race from about 29 s). Not seen: the stacked 2 player views, a backward or mirrored race, a Rush 1 or 2049 track.

### 7.3 Firing backward (Settings, Fire Backward)

Not in 2049. With the Fire Backward option (Games tab, on by default, takes effect at once), holding the steering
stick back while firing (the D-pad's down when it steers, the keyboard's down arrow; `rush2::controls::battle_back`
from `get_race_input`, stick y <= -0.5) shoots behind the car, in the arenas, a race with the BATTLE row and with the
Weapons cheat. It applies to the weapons that shoot ahead: the cannon, gatling, grenade, missile, rocket and gun (the
mine already drops behind, the sonic blast is a ring, the ram has no shot). `fire(rdram, car, backward)` turns the
shot half a turn, levels it (the guns' own aim is at cars ahead), puts its start at the muzzle mirrored behind the mount,
and drops the car's speed ahead from what the shot inherits (a grenade or rocket would otherwise fly forward from a
fast car; the missile's start speed is at least 165 ft/s). The computer cars fire ahead only. Seen in game: player 1's
grenades thrown behind the car (`R2_BATTLE_TEST=log,give2,fire,back` logs each shot's heading against the car's).

## 8. Known differences and test aids

- Computer opponents are the port's own (section 9); 2049's battle was multiplayer only.
- **Ram**: `rush2_battle_ram` (src/rush2049/battle.cpp) runs at the start of Rush 2's
  `car_car_collision_response_8006E2A8` ($a1, $a2 the cars; 2049's `func_800CE358`) with 2049's rule, from the cars'
  current position and matrix (2049 uses the step's snapshot). `ram_box` lengthens the car's box while it holds the
  ram: Rush 2 corners +0xE8 (front z +0xF0, +0xFC), radius +0x658, written only at car setup (`func_8008DBA0`,
  `func_8009E6DC`); its own front + 3 ft, the radius grown to match, and put back when the ram goes (DROP and wrecks
  too). It works with the Weapons cheat as well. Before this the port polled cars within reach closing at 20 ft/s,
  either way round, with a 1 s cooldown and a made-up speed scale.
- **Power-ups**: `update_powerups` ports the three managers (section 5) over the POWUP pickups, swapping the spot's
  scene node to `WEPICON_HEALG1`, `WEPICON_SHLDG1` or `WEPICON_INVSG1` (all in the converted arenas). If no spot is
  free it tries again the next step (2049 would search forever). Before this every POWUP was out from the start and
  gave a random one of the three, back 60 s after it was taken.
- The results print PLAYER 1-4, not profile names; the coin's scale, the
  bullet hit flashes and the non-weapon model colors are by eye **[I]**; an invisible car is a ghost in its own view,
  not a fade.
- Seen in game: 1 and 4 players in DM1 and DM5 (missiles, mines, kills, wrecks, invisibility, the explosion, the
  clock, the results, the arrows), the Weapons cheat in a 1 player practice race, the arrows in a 4 player practice
  race, the track select bands. Not seen: a pickup's light up close, two players stacked and side by side with the
  new renderer, HUD Placement Original and 16:9, teams in play.
- Test aids (env `R2_BATTLE_TEST`, any of): `fire` presses every player's FIRE twice a second; `give<n>` gives every
  car pickup kind n (0-7 weapons, 8 heal, 9 invisibility, 10 shield) at 2 s; `kill` destroys car 0 at 8 s; `short`
  makes the battle 25 s; `log` prints the clock, sounds, pickups and each power-up placed; `models` lists the arena's
  model names; `powerups` places all three power-ups after 1 s at the free spots nearest car 0.
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
  twice as close to), or roam (the best of 12 random open nodes over 80 ft off: farthest from the other cars and
  from where other opponents roam to, plus up to 60 ft at random). A goal that can't be reached is left for 5-10 s.
  They spread out: a car is worth 14 less for each other opponent after it and 8 less for each car within 30 ft of
  it, and a pickup 40 less when another opponent closer to it is going for it.
- **Personalities**: each opponent draws its own at the round's start (`Personality`): -15..15 added to its attack
  and pickup scores, the range it shoots from (25-55 ft), the share of health (0.2-0.5) under which it leaves cars
  healthier than it alone (-35), and 0.9-1 of the skill's top speed.
- **Firing** (the battle's own `fire`, through the same buttons as a player's, `rush2::battle_ai::buttons`): gun and
  gatling when `update_aim`'s yaw is within the target's half width x the skill factor, under 350 ft; cannon and rocket
  when the car points at it (the rocket's flight led), under 400 ft; missile when the target is in a cone ahead
  under 320 ft; grenade within 60 ft plus the car's speed x 0.9; sonic within 55 ft; mine when a car is behind
  within 90 ft. Not through walls (the way 2.5 ft up must be open, except for the lobbed and area weapons). The gatling
  holds FIRE; the others let go a tick between presses. Easy fires at 45% of its chances, Medium 80%.
- **Speed**: the top speed (130 ft/s x 0.78 / 0.9 / 1 by skill) times how little the car must turn (down to a
  quarter). Shooting at a car within its range + 25 ft it holds its range back at that car's speed once it faces it,
  and keeps going round at 35 ft/s until then (a car turns only while it moves); with the ram it drives into it.
  With the gun, gatling, missile or grenade (which needn't point straight at the target: the guns turn about 26
  degrees) it heads, within 1.8 x its range, for a point beside the target (0.4 x its range, at most 16 ft, to the
  side it picks every 4-9 s; the other side if a wall is there), so it circles the car. Other cars within 30 ft
  ahead and 12 ft to either side push its steering away from them (not a car it rams).
- **Recovery**: a car with the throttle on that hasn't moved for 0.8 s, or (not chasing a car) whose goal is close behind it, reverses
  with opposite lock for about a second; one that has had to twice in a few seconds (in a pile) leaves the cars
  alone for 3.5 s (attack scores -60) and its target for as long. One that hasn't got 15 ft anywhere for 10 s (3 s on its roof) is wrecked
  (car +0x648), and the game respawns it.
- **Respawn**: Rush 2049's battle rule (ported, checkpoints.md section 8): every car comes back at the route point
  farthest from the other cars, standing. Before it was ported every car came back at the same point, and the
  opponents gathered there. With `R2_BATTLE_TEST` each respawn is logged (`car N respawns at route point P`).
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
  to all cars with `give<n>` (cannon, grenade, mine, missile, ram, rocket, sonic, and the gun). With the respawn rule,
  spreading and personalities (DM1, 3 opponents, 130 s): 10 respawns at 8 different route points; pairs of opponents
  within 30 ft of each other in 8% of the log's samples (39% before), stopped 7-12% of the time (14-46%), mean speed
  44-55 ft/s (15-33). The BATTLE select's DIFFICULTY row and slider were seen and
  moved. Not seen: how DIFFICULTY 0 and 5 play, the other arenas, 2-4 players with opponents, the gatling.
