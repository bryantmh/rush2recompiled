# Rush 2049 stunt scoring

Port: `src/rush2049/stunt2049.cpp` (Games tab > Stunt Scoring: Rush 2049). Addresses are Rush 2049's (N64 USA) unless a
line says Rush 2. Tags: **[V]** verified in code, **[I]** inferred.

## 1. Where the code is

Main segment, all **[V]** from disassembly (`python tools/rush2049/decomp.py 49 800C2BE0-800C3578` decompiles the per
frame update whole: its labels at 0x800C30B8 and 0x800C3408 are mid-function, not function starts).

| Function | Role |
| --- | --- |
| `func_800C2BE0` | Every frame of a stunt round (game type 4, called from `func_800FA9B4` at 0x800FA9FC): each player's trick detectors, air time, two-wheel tricks and the landing check |
| `func_800C2944` | Roll detector (turn about z): tricks 2 (left, negative) and 3 (right) |
| `func_800C26C4` | Helicopter detector (turn about y): trick 4 |
| `func_800C2430` | Flip detector (turn about x): tricks 0 (back, positive) and 1 (front) |
| `func_800C220C` | Front wheels only: trick 8 |
| `func_800C2004` | Back wheels only: trick 7 |
| `func_800C1B60(trick)` | A trick, or trick 11, the landing that scores the combo; $t1 = the player |
| `func_800C3578(player)` | A wreck: drops the combo (called from the wreck code `func_800C54F0`, `func_800E6AF8`, `func_800FAC08`) |
| `func_800FB2C8` (0x800FBB34) | Round start: zeroes the scores, detectors get flags 0x15000000 |
| `func_80102448` | The round's results table: per player BEST (+8), TOTAL (+0x14) and each trick's round count (+0x48) |
| Mode overlay `func_80391B00` | The HUD: STNT_* icons (file 67, 32 x 32) sliding in with "X n" counts (icon table 0x80393B40); section 5 |
| Mode overlay `func_80392894` | The HUD's text (overlay HUD table entry 0x80393DE0): counts, the combo's points, the "BONUS X n" flash, the landing's bonuses, the total; section 5 |

## 2. State

Per player, a **detector** at `0x801569B8 + p * 0x7C` and a **score** at `0x80152038 + p * 0x78`.

Detector: +0 flags (1 side two wheels, 2 front wheels, 4 back wheels, 0x10 helicopter, 0x20 roll, 0x40 flip, 0x80
multi-axis, 0x100 airborne; 0x01/0x02000000 x turning + / -, 0x04/0x08000000 y, 0x10/0x20000000 z), +4 roll's turn
(x, y, z), +0x10 flip's, +0x1C helicopter's, +0x28 multi-axis's absolute turns, +0x34/+0x38 air start / end, +0x3C/+0x40
side two wheels, +0x44/+0x48 front wheels, +0x4C/+0x50 back wheels, +0x54 settle time.

Score: +0 shown, +2 crashed, +4 last trick time, +8 best, +0xC combo points, +0x10 last score, +0x14 total, +0x18
trick changes in the combo, +0x1C last trick, +0x20 s16[10] counts, +0x34 shown counts, +0x48 round counts, +0x60 wings
time, +0x64 landing bonus kind, +0x66 bonus kind, +0x68 multiplier, +0x6C shown multiplier, +0x70 new kinds, +0x74
landed.

Inputs per car (the 2049 car is `0x8014A250 + car * 0x808`, the car state `0x80152818 + car * 0x3B8`):

| 2049 | Meaning | Rush 2 |
| --- | --- | --- |
| state +0x368 (from car +0x2E0) | the frame's turn: car +0x2E0 += angular velocity (+0x4C) x dt in `func_800D0424` (game type 4 only), copied and zeroed each frame (0x800E858C) | car +0x40 x +0x718 per step |
| car +0x5EC f32[4] | wheel heights above the ground (0, 1 the front) | car +0x5DC |
| car +0x61C u16[4] | wheel materials, 8 = airborne | car +0x60C |
| car +0x6C4 | respawn, -1 once placed | car +0x6C8 |
| car +0x3F8 | wings out (`func_800E15A0`) | `rush2::wings::gliding` |
| state +0x358 | wrecked | car +0x648 |
| state +0xE8 & 0x100000 (car +0x7D4) | the body was pushed out of the world (`func_800C3880`, inside the wall push `func_800C36A0`) | the same push in `car_wall_collision_query_8006FC94`, 0x8006FF60 |

## 3. Rules

Constants at 0x80123EE4-0x80123F50.

- **Turns** (flip x, helicopter y, roll z): a detector starts while the player can trick (fewer than 3 wheels down, not
  wrecked or respawning) and its axis turns fastest; it adds the frame's turn each frame until the direction of its axis
  flips or 3 wheels are down. It ends if another axis turns past 27 degrees (0.4712; a roll allows 72 degrees, 1.2566,
  of pitch). Each **half turn** counts: 144-216 degrees (2.5133-3.7699), or past 99 degrees (1.7279) when cut short (the
  turn reversed, a wheel touched or the body hit); then the turn takes off pi and the others are zeroed, and the
  multi-axis detector stops.
- **Multi-axis** (trick 5): absolute turns summed while no wheel is down; 2 or 3 axes past 27 degrees and a sum of pi
  count once, and stop the flip, roll and helicopter detectors.
- **Air** (trick 9): once per second in the air past 5 s, counted when a wheel or the body touches.
- **Two wheels**: one side's two wheels down (6), only the back wheels (7, a wheelie), only the front wheels (8, by
  wheel heights or contacts); each counts once per 0.15 s past 0.25 s. As compiled, the front-wheels detector starts
  whenever wheel 3 is 2.5 up (`func_800C220C`'s branches), and its end test stops it the next frame if that was wrong.
- **Points**: 5 a trick, 3 for multi-axis and side two wheels. The multiplier goes up by one for each new kind of trick
  after the first change of trick; it is 0 at the round's start (zeroed) and 1 after every combo.
- **Round start bug**: the round's start (`func_800FB2C8`) zeroes the score, so the last trick (+0x1C) is 0, a back
  flip: a back flip as the round's first trick isn't a change of trick and starts no combo (its points wait for the
  next trick of another kind), and the multiplier's 0 gives the round's first combo one less than later ones. The port
  starts a round as after a combo (last trick 11, the landing; multiplier 1), with no option.
- **Landing**: with 3 wheels down, a combo going and the air time ended more than 1 s ago, trick 11 scores it (unless
  the combo was lost to a wreck):
  - flips, rolls or helicopters of a single kind (air allowed) with at most 1 s of wings: helicopters x15 more, flips
    or rolls x25 more;
  - else, with air, multi-axis or turns: no wings x7 more, wings under 3 s x2 more;
  - 3 to 10 kinds of trick: 10, 20, 40, 60, 80, 100, 500, 1000 more;
  - score = points x multiplier (when it is 2 or more), added to the total; the best kept.
- **Wreck** (`func_800C3578`): the combo is marked crashed (no score) until the HUD has shown it (2.69 s since the last
  trick, overlay 0x80394F4C) with no trick going.
- **Round end** (not ported): with the round's time up (0x8013FECB), 0x80152738 is set once no player has had a trick
  going for 2.69 s (0x8016139C).

## 4. The port

`rush2_stunt2049_update` runs in place of `car_stunt_scoring_update_80065588` (once per physics tick per player; each
step's turn instead of a frame's). Rush 2 doesn't run its scoring for a wrecked car (+0x648), so a wreck is caught where
it starts, as 2049 calls `func_800C3578` from its wreck trigger: `car_wreck_trigger_8006E1A8` past its 5 s guard
(0x8006E210, the counterpart of 2049's call at 0x800C5530) and `race_wreck_respawn_wait_8008E40C` where a car's wreck
begins (0x8008E4C0, wreck state 0 with +0x648 set; 2049's crash at 0x800E6DB0). A scored combo shows on Rush 2's stunt panel
(`hud_widget_text_cb_player_stats_800B7654`): the trick's name with its count, or COMBO (Rush 2's own string), over
points X multiplier; BEST and TOTAL are 2049's. Hooks in that callback: 0x800B7838 points the name table slot
(0x800C4BAC + language x 0x14 + 4 x 4) at the name while it is measured and printed, 0x800B78C0 puts it back,
0x800B78F4, 0x800B7974 and 0x800B79F8 rewrite the points line, BEST and TOTAL in the sprintf buffer ($s5). Rush 2's
total (u16) and best (u8) get clamped copies for its post-race and records (`record_stats_race_end_800A9250`); the
profile's BEST STUNT is a byte (`record_stat_stunt_max_80065360`), so it takes the best clamped to 255, and its 2
WHEELS, AIR TIME, FLIPS and ROLLS counts (`record_stat_stunt_count_8006544C`) take 2049's two-wheel, air, flip and roll
counts. The panel shows the landed combo, with Rush 2049's trick names (SPIN, TWIST, 2 WHEEL, WHEELIE, ENDO and the
flips, rolls and AIR TIME); Rush 2049's own icons and live counts are the Rush 2049 Stunt HUD (section 5).

`RUSH2_STUNT_LOG=1` prints every trick and scored combo. Test: `python tools/test_race.py 11 --games
stunt_scoring=Rush2049 --env RUSH2_STUNT_LOG=1`.

## 5. The HUD

Rush 2049 draws the stunt HUD from its mode overlay (ROM 0xB6FEC4, raw deflate, loaded at 0x8038A400; `python
tools/rush2049/decomp.py ovl ADDR`), every frame of game type 4. **[V]** unless marked.

`func_80391B00` (type 4), the icons: ten slots per player at 0x803950C0 + player x 0x2D0, 0x48 bytes each: +0 trick
(0xB free), +2 the icon's model (STNT_xxxG1, icon table 0x80393B40: 0 FLIP_B, 1 FLIP_F, 2 ROLL_L, 3 ROLL_R, 4 HELI, 5
MULTI, 6 2WHL_S, 7 2WHL_B, 8 2WHL_F, 9 AIR), +0x34 alpha, +0x38/+0x3A position, +0x3C/+0x3E start, +0x40/+0x42
target, +0x44 a 0.5 s slide. While the combo shows (score +0 set and either less than 2.69 s since its last trick or a
trick going, +0x18), each kind of trick with a count gets a slot: it slides from the bonus line's place (table
0x80115F28) to the icon place (table 0x80393FD0), both moved 28 px up with one view (16 with two, 12 with more), and
the older slots' targets move 18 px up (one view; else 8). The counts shown (+0x34) follow the combo's (+0x20) until
it lands (+0x74). A crashed combo's icons fade, alpha = (1 - (now - last trick) / 2.69) x 255. The icons are scaled
0.005 (one view) or 0.0035. Once the combo no longer shows and no trick is going, the score's display fields are
cleared (+0, +2, +0x10, +0x34 counts, +0x65, +0x67 zeroed, +0x6C = 1) and the slots freed.

`func_80392894`, the text, in color 1 (`func_800B74A0(1)`: 0x80118E34 + 8, 0xE6E6E6 with a 0x3C3C3C shade), left
aligned and centered on y (`func_800B669C(0, 1)`), in font 10 (4BAERO-PLAIN12) with one view, else font 12
(AERO-PLAIN7):

- each slot's count, "X %i" (0x80394A88), 12 px (one view; else 6) right of the slot, at its alpha;
- the combo's points "%i" (+0x10) centered 12 (6) px right of the icon place; while the combo is going +0x10 takes
  its points (+0xC) every frame;
- "BONUS X n" (string 181 and "%s X %i", n = +0x6C, the multiplier before the landing's bonuses) centered on the bonus
  line's place for ten frames for each new kind of trick (+0x70, counted down; frame counter 0x80395ED8 + player x 4);
- once landed and not crashed, lines down from the bonus line's place: "BONUS X n" if n is 2 or more, the bonus kind's
  name (+0x67: 3 TRIPLE BONUS X10 up to 10 PAPYRAY BONUS X1000, 11-13 PERFECT SPIN / ROLL / FLIP BONUS), and the
  landing's (+0x65: 1 NO WINGS BONUS X7, string 179; 2 PARTIAL WINGS BONUS X2, string 180);
- the total "%i" (+0x14) centered on table 0x80393F50's place.

The icons' size: `func_800A6094` puts each one 2 units in front of the camera at its screen point, and its model
(STNT_xxxG1: a 14-sided disk over the whole 32 x 32 texture, radius 320 model units, 16 to a unit) is scaled 0.005
(D_80394F44; 0.0035, D_80394F40, with two views): radius 0.1 units. With the 90 degree view (`view_set_fov_800A5744`;
320 px wide, so 80 px a unit at 2 units) that is **16 px across** with one view, 11 px with two, and 8 px in a 160 px
wide quarter view: half the one-view size, as the text's AERO-PLAIN7 is about half 4BAERO-PLAIN12.

**The total's box** (`func_803925D0`, the overlay's setup, game type 4): one per player, centered on the total's place,
as wide as "8888888" (0x80394A80) plus 8 px and a line plus 4 px high, in font 10 (one view), 11 (4BAERO-PLAIN10, two)
or 12; made by `func_800B3B4C(x0, y0, x1, y1, 0x80, ...)`: a fill of (0, 0, 16) inside a 1 px border of (0, 0, 192),
both at alpha 0x80 (`func_800B39BC` places the five rectangles). Nothing else in the stunt HUD has a box: the icons,
counts, points and bonus lines float over the view.

Places (x, y) by view count, players in order: icon place 0x80393FD0: one view (264, 208); two (208, 98), (208, 208);
three or four (128, 98), (172, 98), (128, 208), (172, 208). Total 0x80393F50: (160, 208); (120, 98), (120, 208); (80,
98), (240, 98), (80, 208), (240, 208). Bonus line 0x80115F28: (160, 110); (120, 55), (120, 165); (80, 55), (240, 55),
(80, 165), (240, 165).

Text helpers (main segment): `func_800B42F0(n)` selects font n, `func_800B74A0(i)` the color, `func_800B669C(h, v)`
the alignment (0x80118E20, 0x80118E24; v = 1 centers on y), `text_center_x_800B71A0`, `text_draw_800B71D4`,
`text_width_800B3FA4` (a character's cell width + 1 + the gap, 0x80149B70 = glyph table byte 8; a space is byte 7)
and `func_800B3F50` (a line's height). The glyphs are drawn with a palette (TLUT load in `func_8008A148`) built from
the color and its shade **[I]**.

### The port's Rush 2049 Stunt HUD

Part of Stunt Scoring: Rush 2049 (with Rush 2049's ROM loaded; without it Rush 2's panel shows the scores, section 4).
`rush2::stunt2049::draw_hud`, called from `rush2_hud_draw_end` in src/hud.cpp. Rush 2's stunt boxes are hidden
(`rush2_stunt2049_box_visible`, a hook at 0x80060E98 in `hud_stunt_box_visible_80060E34`, which also keeps whether they
would show) and so is their text (`rush2_stunt2049_panel_text`, 0x800B76CC in `hud_widget_text_cb_player_stats_800B7654`);
Rush 2049's HUD is laid out from where the HUD layout puts the boxes (`rush2::hud::stunt_place`: each box's place,
scale and anchor), at Rush 2049's one-view sizes times the box's scale (16 px icons, 4BAERO-PLAIN12; at the half size
of 3 or 4 players AERO-PLAIN7, as Rush 2049 uses with more than one view):

- the trick name's box: a row of icons with their counts along its bottom, newest first, sliding in over 0.5 s; the
  combo's points above them; above those the "BONUS X n" flash or the landing's bonus lines, which reach above the
  box when there are several (Rush 2049 stacks its icons upward from its icon place too). It shows while Rush 2049's
  overlay would show the combo;
- the scores' box: Rush 2049's total box and total in its middle; when the layout puts it under the trick name's box,
  only while the combo doesn't show.

The fonts are drawn from their sheets as I4 textures (`rush2::hud::draw_i4_part`; IA8 AERO-PLAIN7 keeps its coverage
nibble) loaded whole and clamped, so the High-Resolution Fonts pack replaces them by hash: `python
tools/build_font_pack.py trace-2049` redraws them (tools/font_pack/svg_2049) from the 2049 ROM. Fonts and icons are
read on a worker thread (include/font2049.h; file 67, whose CI4 icons have no palette records: each takes the 16
colors after its texels, as its TXLD load list does) and copied once to 0x80F18000-0x80F20000 (below 16 MB: texture
addresses are 24 bits). The icons get a round mask, as their models are 14-sided disks. The text's color goes from the
shade to the color with the texel's intensity **[I]**: Rush 2049's palette isn't ported. `RUSH2_STUNT_LOG=1` also
prints each change of what the HUD shows (`[stunt2049] pN hud:`).
