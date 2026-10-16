# Race HUD layout

The race HUD's place on the screen is decided by one engine, `src/hud_layout.cpp` (API in `include/hud_layout.h`),
from two things only: the race's settings (`rush2::hud_layout::Config`) and the HUD catalog (`src/hud_catalog.cpp`,
`include/hud_catalog.h`), a list of every part the race HUD can draw and where the game draws it, derived from the
ROM's layout tables and the game's callbacks. Nothing comes from what the game drew, so `tools/hud_preview` lays out
and draws any variant (1-4 players, stacked or side by side, every aspect ratio, odometer, gearbox, stunt panel,
stunt arena, DEATHS, each HUD setting off) exactly as the game will, with the ROM's images, without starting it.
`src/hud.cpp` only applies the engine's result to the game's widgets, and logs any widget the catalog doesn't cover.

## How the layout works

The game builds the race HUD as 2D widgets laid out for one view or two views stacked top and bottom
(`hud_race_setup_widgets_800A06F8`; players 3 and 4's are copies of player 2's, `src/players4.cpp`).
`rush2::hud_layout::solve(config)`:

1. Takes the catalog's parts for the race (`hud_catalog::parts(variant_of(config))`: the player count, views,
   language and the time setting change where the game draws some of them).
2. Groups them into elements: the parts of one role, player and group (a player's time, speedometer, radar...; the
   shared track map, time left; each message on its own, WRONG WAY with its three arrows). An element's bounds are
   its frames' opaque texels plus everywhere its moving parts can go (the tachometer's needle, the radar's dots, the
   finish flag, WRONG WAY's slide).
3. Decides which elements **can appear in this race** from the settings alone (`possible()`). Every element that
   can appear keeps its place all race, shown or hidden, so nothing moves when something appears or disappears, and
   nothing is ever placed over something that could show there. An element that can't appear takes no room: the
   tachometer and gear with an automatic gearbox, the stunt panel without stunts, the skull without DEATHS.
4. Places each element by its hand placement (below) if it has one, else by the first rule for its role and layout
   in `rules()`, read top to bottom.
5. Returns, per part, its move, RT64 origin and scale; the odometer's place; each view's Rush 2049 battle HUD
   (below); and for each player whether the trick name's box is placed over the stunt scores' (`stunt_swap`: the
   scores' box is hidden while the trick name's shows).

### Battle HUD

Rush 2049's battle HUD (`src/rush2049/battle.cpp`) has no Rush 2 widgets: the health bar is a widget the recomp
builds, the weapon held, the power-up and the coin are 3D models placed at screen points, and the ammo and kills are
drawn by `rush2::hud::draw_number`. The engine places them (`place_battle`, after everything else) as Rush 2049 does
(its tables 0x80394150, 0x803941D0, 0x803940D0, 0x80394210: the bar centered at the bottom of the view, the weapon on
its outer side, the coin toward the screen's middle; a third wider with two players), as virtual elements
(BattleBar, BattleWeapon with its ammo, BattleCoin with the kills, BattlePowerup) so `check()` and the previewer see
them with the rest; battle.cpp reads the places with `rush2::hud::battle_place`. `Config::battle_hud`
(`rush2::battle::hud_mode`) picks the HUD: a battle arena's; a race with the BATTLE row or the Weapons cheat, one
layout so the bar never moves between them (`battle_race_place`: the bar; the weapon held with its ammo to its right
(not with the cheat: endless) and the power-up. Widescreen (and two side by side at any aspect): beside the bar,
2 px from it, their bottoms lined up with its bottom, the weapon left and the power-up right in every view (a
battle arena's right-hand views mirror that, as Rush 2049); on the time and position's row slid sideways into the gap
between them if it's too wide where it is; else lifted over them, else the three centered together on the view; the
first clear of
the player's HUD and the track map (and, with one view, Rush 2's stunt boxes; split screen leaves them to
attachments), inside the view's 5 px margin. Two stacked: the stunt boxes don't take part in that choice; the row
(weapon and ammo, bar, power-up) is pushed right until it's 2 px clear of the stunt box. At 4:3 (but two side by
side), or with no room beside: stacked, the weapon 2 px over the bar, centered with its ammo (else the model alone),
the power-up 2 px right of the ammo; the first clear, else the two slid sideways the least that's clear (up to half
the bar). An arena's coin where Rush 2049 has it would run into the speedometer, tachometer or gear (the bottom row of
quadrants in 4:3) goes 2 px under them (`coin_clear`). Two side by side: the bar at the bottom of the view.
battle.cpp draws the cheat's weapon held too). In a race the bar on the player's time and position's row (split
screen) goes between them, off center if need be (2 px from them, else 1, else touching), else above them. In split screen each bar is attached (built in, `src/hud_overrides.inc`, so
the editor can undo it) up and down to its trick name, 2-3 px over it (3 or 4 players at 4:3: players 3 and 4's 24
px over it, `only_43`, clear of the lap time's row): with the stunt boxes shown the bar goes over them, and the weapon and power-up, over the
bar by rule, go with it; with them off the attachment does nothing. In quadrants the whole battle HUD is drawn at
`small_scale` (the bar's frame, the models, the digits), as the rest of the HUD there, and an arena's power-up goes
over the bar's inner end (beside it, it would meet the coin). The models' sizes on the screen aren't known outside the game, so
their boxes are estimates (24 pixels, the power-up 20, 70% in a quadrant, times their scale).

A battle arena hides Rush 2's HUD but the speedometer, tachometer, gear, the time left (with a time limit) and the
finish overlay (`rush2::battle::hud_built`), so nothing else is possible there (no odometer either).

With one view or two stacked, nothing moves: each group of elements whose frames touch (one player's, or the
shared ones; a moving part's reach doesn't join groups) gets the origin of the screen third its center is in (left
third: the window's left edge, right third: its right edge, else centered), and only the odometer is placed. This
is the HUD as it always was.

`src/hud.cpp` solves again only when the settings change. Each widget is matched to its part by how it was built
(below) when it is built; each frame its part's place moves it for the draw loop, picks its origin and scales it.

### Rules

A rule names up to three places, tried in order. The first place that's clear of every element already placed
(except the pairs in `allowed_overlap`) and inside the player's part of the screen is taken; if none is, the least
bad one is and the element is marked cramped (an error in `check()`). Places are described the way a layout is:

- **Sides**: left, right, outer (toward the screen's edge: left in the left column, right in the right), inner,
  centered in the player's half or quadrant, centered on the screen.
- **Rows**: edge (the screen's top; the bottom row of quadrants: its bottom), mid (the line between the views),
  top, bottom, centered.
- **Against other elements**: between two (the speedometer between the time and position), lined up with one's outer
  or left edge, keeping the game's offset from one (the tachometer from the speedometer), beside one on its inner
  side, past one toward the line between the views (`After`) or toward the screen's edge (`Before`), centered on one.
- **Stacks**: elements placed one after another from a corner: CornerLeft and CornerRight from the edge corners
  (growing toward the line between the views), MidOuter from the outer corner on the line between the views (growing
  toward the edge), BottomOuter from the bottom outer corner (growing up). An element joins a stack after its last
  item, a gap away.

Positions are worked out on the screen as drawn: 4:3 coordinates widened to the HUD's width
(`rush2::splitscreen::hud_width`, from HUD Placement), so an element against a window edge moves out with it in
widescreen. Each element takes the RT64 origin of what it's placed against: a window edge, the middle of its half
(an origin at a quarter of the window), or the screen's center for the inner side. Every player's part of the
screen keeps a 5 pixel margin from its edges and the lines between the views.

The split screen rules, in order:

| Role | Layouts | Places |
|---|---|---|
| Map, LapsLeft | split | keep their place (first, so each player's HUD is placed clear of them); the flag on the map |
| Time | split | stack CornerLeft |
| LapTime | split | stack CornerLeft after the race time, the game's gap, x kept from the race time |
| Position | split | stack CornerRight |
| Speed | top row | its plate centered on its view (X ViewCenter: anchored at the view's middle, exact at every aspect) at the top; the same under the time and position |
| Speed | bottom row | its plate centered on its view at the top of the quadrant |
| Tach | split | its arc (not the needle's travel) centered on its view, at the game's height from the speedometer |
| Radar | split | stack MidOuter |
| Odometer | quadrants, race | stack MidOuter after the radar, against it (0 px), centered on it (X CenterOn); else stack CornerLeft after the time |
| Odometer | side by side | stack MidOuter after the radar (2 px); else stack CornerLeft after the time |
| Deaths, It | top row | stack CornerOuter (5 px); beside the radar (and odometer), centered on the radar; beside the radar and odometer under the corner panel |
| Deaths, It | bottom row | stack MidOuter (5 px); beside the radar, centered on it; beside the corner panel, centered on it |
| Gear | split, a race at 4:3 | lined up with the inner panel's outer edge, 5 px past the time and position, then past the odometer too (beside the tachometer it would meet the position) |
| Gear | split | where one player's game has it beside the tachometer: 100 right of the tachometer, 15 higher (player 1's offset for every player; the game puts players 2-4's 56 below theirs), at the tachometer's scale, always (the overlap with the needle's swing is allowed). Without a tachometer: lined up with the inner panel's outer edge, 5 px past the time and position, then past the odometer too |
| StuntScore | split | X Pair (`stunt_pair`, below: centered at the bottom of the view); else stack BottomOuter |
| StuntTrick | split | the stunt scores' place, at their scale (shown instead of them while a trick is up) |
| Odometer | quadrants, stunt track | stack CornerLeft after the time (player 1); else stack BottomOuter (the bottom outer corner); else stack CornerLeft |

The stunt boxes in split screen (2 side by side, 3 or 4 players; a race with Stunts on All Tracks and the stunt
track): there's no room for both, so the trick name takes the stunt scores' place and the game shows it instead of
them while a trick is up (`stunt_swap`, src/hud.cpp), as one box. The scores (`stunt_pair`, X Pair) go at the bottom
of the view (the top row: on the line between the views), 5 px margin, centered on the view, moved only as far as
needed to keep 2 px (else 0) from the elements on that row (the race time and position, the radar, the track map).
Two stacked views do the same (`solve_single`): one box per view at 80%, 2 px right of the radar with its bottom on
the radar's (the stunt track, no radar: in its place), the trick name in the scores' place. One view keeps Rush 2's
two boxes.
| Banner | split | centered in the player's half or quadrant |
| Map | split | its place in the half or quadrant (centered on the line, or the cross, between the views) |
| LapsLeft | split | keeps the game's offset from the track map |
| TimeLeft | side by side | centered on the screen 2 px over the track map and the finish flag; else at the top |
| TimeLeft | quadrants | centered on the screen 2 px over the track map and the finish flag; else on the cross |
| Odometer | one view or stacked | lined up with the radar's outer edge, over it (2 px); else under the time |

Elements without a rule (the stunt panel side by side, messages) keep their place in their half or quadrant: moved
from where the game puts them for stacked views to the same place in the player's half by thirds (near an edge they
keep their distance from it, in the middle they stay centered).

### Scale

With 3 or 4 players the skull, the IT badge and the stunt boxes are drawn at half size. `small_scale` is
`fitting_scale()` in `src/hud.cpp`, the lap timer's: the scale nearest 75% at which a texel covers whole output
pixels (2/3 at 720p and 1080p, 5/6 at 1440p; the previewer uses 0.8). Every other element in quadrants (3 or 4
players: the same layout) is drawn at it, the odometer too (its element's scale; `odometer2049::draw` rounds a digit
to whole pixels). Each element is scaled about its top left corner; one that keeps its place (the messages, GAME
OVER) keeps its center. The laps-left number keeps its size, but its place scales with the finish flag. The gear
is drawn at its tachometer's scale, and with one view or two stacked follows its tachometer's scale and place when
that's scaled or placed by hand (staying where the game has it beside it). With two stacked views player 2's
gear is beside its tachometer at player 1's offset, as the game draws it (its ROM table entry, 56 below the
tachometer, isn't where it's drawn).

### Allowed overlaps

The finish banner and the messages (WRONG WAY and its arrows, the bonus time banner, GAME OVER) may overlap
anything; the trick name's box takes the stunt scores' place; the tachometer hangs from the speedometer; the finish
flag sits on the track map. Any other overlap between elements that can show is an error in `check()` (with one
view or two stacked, the game's own layout, a note). The skull and the IT badge must stay clear of the lap time box:
it shows after every checkpoint and would hide them.

### Sliding elements

WRONG WAY (`hud_widget_cb_lap_banner_800BA868`) moves. It shows at (88,50) (player 2 and up: (88,160)) with its
three arrows under it at x 160 - 16k, 16 lower, blinking (alpha 0x60 / 0xFF by the time since it showed: (t mod
350) / 50 against the arrow's index). With one player, from 3 s to 3.5 s after it shows it slides in a straight
line to (22,192), the arrows to x 60 - 16k, 142 lower (`f = (t - 3000) / 500`, x = start + f (end - start), y =
start + 142 f); with two or more players (`race_player_count_8010C3E2` >= 2, read at 0x800BAB00) it hides at 3 s instead: Rush
2's own behavior, which the recomp keeps. Its parts carry the slide's end (`Part::end`).

The text and its arrows are one element, so they always move together. With one view the group is anchored by the
screen third it's in at each end of the slide (`Element::origin`, `origin_end`: the middle at the start, the left
edge at the end), and while it slides the game blends its RT64 origin from one to the other by how far along it is
(`slide_origin()`, from where the widget is drawn this frame), so in widescreen it glides from the middle to the
window's left edge instead of jumping. Split screen it never slides; it keeps its place in its half or quadrant.
The previewer draws the slide's end faded with the path dotted; the HUD editor plays it.

### Hand placements

`src/hud_overrides.inc` (written by the HUD editor, below; built into the game and the previewer by
`builtin_overrides()`) holds hand placements: for one layout (`LayoutId`: 1 player, 2 stacked, 2 side by side, 3
or 4 players: 3 players are 4's quadrants with the last one empty, so they share P4's; P3 is never used) and a race
mode (`HudMode`: every mode, or one of race (a race, circuit or practice, with or without the BATTLE row or the
Weapons cheat: one HUD), stunt track, battle; one for the race's mode, `hud_mode()`, is taken over the one for every
mode), an element (role, player, group) is `moved` to (x, y) relative to an RT64 origin, `attached` to another
element (x, y from that one's top left corner, wherever it goes: both ways, with its anchor, or one way,
`attached_y` up and down / `attached_x` sideways, keeping its own place the other way; nothing while that one isn't
shown; `attach()` after the rules, chains followed; the battle HUD's in `battle_attach` after its hand placements), `scaled` (times its rule's scale, so it keeps up with `small_scale`), and/or
`hidden`; a placement with `only_43` is used only with the HUD at 4:3 (over one for any aspect; the race mode's own
still comes first). A line's 10th field is its mode (`HudMode::Stunt`; left out, every mode), and an attached one's 11th-13th
the element it's attached to (`Role::Radar, 0, 0`). A moved element is placed there instead of by its rule, before the elements placed after it, so those are
placed against it where it now is (move the speedometer and the tachometer follows). A hidden one takes no room and
isn't drawn: `src/hud.cpp` hides its widgets for the draw loop and sends text printed over it off the screen;
the battle HUD's are skipped by `src/rush2049/battle.cpp` (`BattlePlace::show_*`). A battle HUD element moves its
places in `BattlePlace` with it and takes the placement's anchor; its scale scales the bar's frame or the model.
Positions are relative to the anchor, so one placement works at every aspect ratio.

Hand placements in the 1 player and 2 stacked layouts change base Rush 2's HUD, so per the project rules they need a
toggle in the game's options before they ship; the editor warns when it has any.

## Catalog

`src/hud_catalog.cpp` lists every part the race HUD can draw. `parts(variant)` returns them in the order the game
builds them:

1. The bonus time banner's box (`hud_multi_widget_menu_setup_80093B38`, built first by the HUD setup).
2. The stunt boxes (`src/players4.cpp` `stunt_boxes`): one player at y 180-220; otherwise player 1 at y 70-110 and
   player 2 at 180-220. The scores' box x 0xC1-0x12A, the trick name's 0x16-0x7F; callback 0x80060E34, parameter
   `(player << 4) | kind`. Their text is printed inside them by 0x800B7654 (Text parts).
3. The 112 entries of the race HUD setup's table (0x800C2DB4), then players 3 and 4's copies of player 2's
   (`players4` `cloned_callbacks`, the player in the parameter).
4. The finish overlay (`hud_race_finish_overlay_build_800A0FB4`). Its boxes come first, so they're drawn behind its
   images: once the race is over (game state 4 or 5) GAME OVER's box, (83,67)-(237,104) with one view, else
   (83,105)-(237,142) (alpha 0xC0, no callback; the GAME OVER element's); in a split screen each player's finish
   box: player 1 (99,29)-(221,63), the others (99,171)-(221,205). The boxes have no callback; players 3 and 4's
   carry their player as the parameter (`players4` `finish_boxes`), as their boxes are player 2's and would
   otherwise be built alike. Then its 7 entries (0x800BF328) and their copies. The boxes are built before
   `rush2_hud_finish_begin`, so `rush2_hud_box_part` counts a box as the HUD's when the race's catalog knows it.

The tables and every image's size and opaque texels come from the ROM: `src/hud_catalog_rom.inc` is generated by
`tools/hud_preview/gen_catalog.cpp` (`build.bat` makes it if it's missing; run `hud_catalog_gen` again to refresh it).
What each callback does with its entry was read from its code; each part's `note` says it. In 4:3 coordinates
("views" is `race_player_count_menu_8010C3EC`, "players" 0x8010C3E2):

| Callback | Part | Where the game draws it |
|---|---|---|
| 0x800B7B1C | tachometer TACH | the table's (112,29), player 2 (112,139); the player in the parameter's low nibble |
| 0x800B7E88 | TACHINSIDE (parameter >> 4 = 1) | inside the tachometer |
| 0x800B7E88 | TACHNEEDLE | x = 157 + 63 cos a clamped to 112-204, y = 1 - 41 sin a, at least 30, +110 for any car but the first; a 215-315 degrees. Reach (112,30)-(219,48) |
| 0x800BA2A8 | SPEEDBG (sub 4), DIGIT1 16 x 13 cells, PH MPH/KPH 32 x 7 cells | (133,14), player 2 (133,124) |
| 0x800B9754, 0x800B9978 | TIMETOP (sub 9), TIMEFONT 7 x 11 cells (0-9, ':' 10, '.' 11) | race time (17,15) / (17,125); lap time (17,33) / (17,143) |
| 0x800B94C0 | time left: COUNTDOWNBG, DIGIT0 16 x 17 cells | (250,14), shared; hidden while 0x8010C3F2 is 0 and by No Race Timer (0x800D9E88) outside a battle |
| 0x800BA608 | position: COUNTDOWNBG, DIGIT1 / DIGIT0 cells | (250,37) / (250,205) |
| 0x800B92F4 | gear: GEAR dim; the lit gear (parameter bit 12) a 32 x 6 row at y + 6 x gear | (212,14) / (212,195); hidden while `hud_show_gear_8010C44A` is 0 (read at 0x800B938C), gone for good when the car's +0x3D8 is set (automatic) |
| 0x800BA868 | WRONG WAY, HUDARROW k = 1-3 | WRONGWAY (88,50), player 2 on (88,160); arrows (160 - 16k, base + 16). One player: after 3 s it slides to (22,192), the arrows to x 60 - 16k, 142 lower. The table's positions are never used |
| 0x800B9C44 | CROSSBONES (the DEATHS option `race_deaths_8010BC98`) | x 80 with the time shown, else 12; y 10 / 120; shown while the option is on and player < players |
| 0x800B9CC0 | IT badge: ITBG (82,11), IT (86,15), +110 for player 2 | shown when `race_type_8010C3E8` is 2, `race_game_mode_800FAE6C` is 0, `race_mode_option_8010C169` is set and the player's record +0x21 (0x800C2161 + player x 40) is set; a battle hides it (`rush2_battle_hide_stunt_panel`, 0x800B9D2C) |
| 0x80060F24 | bonus time: BONUSTIME4 and its box | (96,160) and (0x64,0x9B)-(0xDC,0xB5); both 50 higher with 2 or more players |
| 0x800B920C | radar MAPFRAME | (17,70) with one view, else y 52 (player 1) / 162; hidden on track 11 (the stunt track) |
| 0x800B8CC8 | radar dots: TRAFFIC 2 x 4 cells | inside the radar; parked at (-1,-1) while hidden |
| 0x800B8188 | track map TRACKMAPBG4 | (207,72) always; the track is drawn into its texels at run time, so its bounds are the whole image. Hidden on track 11 and while 0x8010C3E4 is 0 |
| 0x800B8900 | map dots: TMAPDOT 4 x 4 cells | inside the map |
| 0x800B9D48 | finish flag and laps left (`hud_sprite_cb_laps_left_800B9D48`) | the finish point X 211-299 (mirrored 210-298), Y 76-164, nudged per track by 0x800C3F84 / 0x800C3FB4 (by track % 12); the flag (a 15 x 16 cell) ends at it, the laps number is printed at (X - 6, Y - 8) (mirrored X + 2) in font 1. Reach (191,57)-(319,164). Hidden on track 11, in stunt and practice, and in the attract demo |
| 0x800B5A90 | finish place: 1STPLACE, ENDPLACE 48 x 24 cells, ENDPLAC1 "PLACE" | x 96 (PLACE at 155); y 144 with one view, else the table's (34 / 176) |
| 0x800B5CAC | GAMEOVER | (88,72), y 110 with two views or more; on GAME OVER's box (finish overlay, above) |

`widgets_create_800604FC(0, 0, table, count)` reads 0x28-byte entries: +0 the image's name (0 none, -1 a widget
whose callback only prints), +4 x, +6 y, +0x1C alpha, +0x20 callback, +0x24 parameter. It runs each callback once
as it builds the entry; callbacks may clear themselves (+0x28 of the element = 0) for absent players and keep state
in the parameter, so a widget can only be told apart by how it was built. `src/hud.cpp` records that as it happens:

- `rush2_hud_entry_created` (0x80060594 in `widgets_create_800604FC`, after the element is built and before its
  callback runs: $s0 the entry, $v0 the element, +0x34 its widget): the entry's image name, x, y, callback and
  parameter.
- `rush2_hud_box_begin` (entry of `hud_multi_widget_menu_setup_80093B38`: x0, y0, x1, y1 in $a0-$a3, alpha,
  callback, parameter and the kept flag at $sp + 0x10-0x1C) and `rush2_hud_box_part` (0x80093C20, after each of its
  five elements: $v0 the element, $s2 which: 0 the top edge, 1 the left edge, 2 the fill, 3 the right edge, 4 the
  bottom edge). The fill is black at the alpha, the edges 0x8C gray.

`hud_catalog::find(parts, identity)` gives a widget's part; `check()` reports two parts with the same identity
(their widgets couldn't be told apart). In the game, `check_widgets` logs once (to stderr,
`[hud] widget ...`) any race HUD widget shown with no part, drawn outside its part's reach, or of a part the catalog
says can't appear in this race: the catalog is missing something, so the previewer would be wrong too. Fix the
catalog, not the layout.

### Images

The images come from assets 0x12 (DIGIT1, TIMEFONT, TRACKMAPBG4), 0x13 (the HUD's own) and 0x16-0x18 (English,
French, German: 1STPLACE, BONUSTIME4, ENDPLAC1, ENDPLACE, GAMEOVER, GEAR, WRONGWAY), LZ compressed
(`rush2::track2049::rush2_lz_decompress`; the asset table is at 0x800C185C). Images are stored bottom up:
`gfx_draw_frame_views_8007D9DC` loads rows `src_y0`..`src_y1` (widget +0x18, +0x1C) from pixel rows
`height - 1 - y` (0x8007EF6C-0x8007EFD0); source rectangles are top down. Format 5 (the big digits) is the game's
own: an intensity the draw tints with the primitive color, texels of 0 transparent.

### Roles

| Role | Parts | Can appear (the game's HUD setting, the race) |
|---|---|---|
| Time, LapTime | 0x800B9754, 0x800B9978 | time 0x8010C3F1; the stunt mode: player 1's race time only, no lap time |
| Speed | 0x800BA2A8 | 0x8010C434 (every player, also in the stunt mode) |
| Position | 0x800BA608 | 0x8010C441; not in the stunt mode (it reads the race type) |
| Radar | 0x800B920C, dots 0x800B8CC8 | radar 0x8010C3EE, not on the stunt track |
| Map | 0x800B8188, dots 0x800B8900 | map 0x8010C3E4, not on the stunt track |
| LapsLeft | 0x800B9D48 | the map, not on the stunt track, a race or circuit, not the attract demo |
| TimeLeft | 0x800B94C0 | 0x8010C3F2, not No Race Timer outside a battle |
| Tach | 0x800B7B1C, 0x800B7E88 | manual gearbox, tachometer 0x8010C425 |
| Gear | 0x800B92F4 | manual gearbox, gear 0x8010C44A |
| Deaths | CROSSBONES 0x800B9C44 | the DEATHS option, not a battle |
| It | 0x800B9CC0 | the stunt mode with its option 0x8010C169, not a battle or the attract demo |
| StuntScore, StuntTrick | the stunt boxes 0x80060E34 and their text | the stunt mode or Stunts on All Tracks, not a battle |
| Banner | finish place 0x800B5A90 and the finish boxes | always |
| Message | WRONG WAY 0x800BA868 (setting 0x8010C452), bonus time 0x80060F24 and its box, GAME OVER 0x800B5CAC | always |
| Odometer | `src/rush2049/odometer.cpp` (no widgets: a virtual element) | the Odometer option; every player (in the stunt mode too, where only player 1 has a race time) |
| BattleBar, BattleWeapon, BattleCoin, BattlePowerup | Rush 2049's battle HUD (virtual elements, above) | `Config::battle_hud` |

With one player the game still builds player 2's widgets (they never show: `present` is false).

## Adding or changing a HUD element

A new element goes through the catalog, so the previewer knows it before the game is run:

1. Add its parts to `src/hud_catalog.cpp`: where the game draws it (`at`), everywhere it can move to (`reach`), its
   frame's opaque texels (`opaque`), its role, player and group, and how to draw it (`texture`, cell or text). A
   game widget is matched by how it's built (its table entry or box); a recomp-drawn element (the odometer) is a
   virtual element in `build_elements`.
2. Say when it can appear in `possible()`, from the settings alone, so it keeps its place from the start (and takes
   no room in races it can't show in).
3. Add its rule to `rules()` after every element it's placed against. If it may overlap something by design, add the
   pair to `allowed_overlap()`.
4. Check every variant with the previewer: `--matrix` draws every layout, aspect and option with everything that
   can appear. In a game run, `[hud] widget ...` lines in the log mean the catalog is incomplete.
5. Where a rule's places don't look right, place the element by hand in the HUD editor.

## Previewer

Build with `cmd //c "tools\hud_preview\build.bat"` (writes `tmp\hud_preview\hud_catalog_gen.exe` and
`tmp\hud_preview\hud_preview.exe`; it compiles the game's `src/hud_layout.cpp` and `src/hud_catalog.cpp`, so a
catalog or rule change needs only this build). It reads `rush2.us.recomp.z64` (`--rom PATH`) for the images.

- `hud_preview [--set KEY=VALUE]... [--show all|none|ROLE,...] [--out F.png] [--zoom N] [--json F] [--parts]` lays
  out one variant, prints every element's rule and place and the issues, and draws it. `--set`: `players=1-4`,
  `layout=stacked|side` (2 players), `aspect=4:3|16:9|21:9|32:9` or `hud_width=W`, `odometer=0|1`, `manual=1010`,
  `mode=race|circuit|stunt|practice|battle` (stunt: the stunt track, the only place the stunt mode is played),
  `race_battle=0|1` (the BATTLE row), `weapons=0|1` (the cheat), `battle_time=0|1`, `stunts=0|1`, `deaths=0|1`, `it=0|1`, `language=0-2`,
  `attract=0|1`, and the HUD settings `time`, `speed`, `position`, `radar`, `map`, `time_left`, `tach`, `gear`,
  `lap_banner` `=0|1`. `--parts` lists every part with where the game draws it, its reach and its place.
  `--overrides FILE` uses those hand placements (`src/hud_overrides.inc`'s format) instead of the built-in ones;
  `--json F` writes every element, every part as drawn (image, cell, slide end) and the issues, `--no-picture` skips
  the picture.
- `hud_preview --dump-images DIR` writes the HUD's ROM images as PNGs (`NAME.png`; French and German `NAME.1.png`,
  `NAME.2.png` where they differ) and Rush 2049's (`2049_ODOMETER.png`, `2049_HEALTHBG.png`), for the editor.
- Rush 2049's ROM (`--rom2049 PATH`, else `RUSH2049_ROM`, else `build/rush2049.z64`) gives the odometer's digits
  (ODOMETER, file 64, read by `rush2::rom2049::file_image` as the game's `src/rush2049/odometer.cpp` does) and the
  battle bar's frame (HEALTHBG, file 63), drawn where the engine puts them. The battle HUD's weapon, coin and
  power-up are 3D models in Rush 2049 and are drawn as shapes. Without that ROM the odometer and bar are shapes too.
- `hud_preview --matrix OUTDIR` draws every variant (1, 2 stacked, 2 side by side, 3 and 4 players; 4:3, 16:9 and
  21:9; odometer and gearbox on and off; a race, the stunt panel, DEATHS, the stunt arena with the IT badge; each HUD
  setting off) with everything that can appear, and lists each one's issues in `OUTDIR/index.txt`.
- `hud_preview --truth CAPTURE` checks a capture of the game against the catalog and the engine: every widget has a
  part, is drawn inside its part's reach and was moved as the engine says. Older captures (before the build
  identity was recorded) are matched by image and callback and only checked for reach.

In pictures each element is outlined and labelled (dashed: not shown in this picture; red: cramped), a moving
part's reach is dotted (a sliding one's end faded, its path dotted), the views are split by lines and the 4:3
screen's edges are dotted.

## HUD editor

`python tools/hud_preview/editor.py [--port 8765]`, then open http://127.0.0.1:8765/ (build the previewer first). The
page shows the engine's result for a layout and options (aspect; mode: race (a race, circuit or practice), stunt
track or battle; odometer, gearbox; and those of the mode only: DEATHS, Stunts on All Tracks, Race battle (the BATTLE row or the
Weapons cheat: one layout) in a race, IT (the stunt mode's badge on the player who is it) on the stunt track, the time limit in a
battle; language) drawn with the ROM's images, every element outlined, and the
issues.

- Drag an element to move it. It snaps (within 2 px) to other elements' edges and centers and to its view's edges,
  margins and center, with the guide drawn; the Snap box or S turns snapping off (remembered), Ctrl or Alt while
  dragging skips it. Arrow keys nudge exactly 1 px (Shift: 5), never snapping. Zoom (2-6) for fine placement.
- Elements in the same place are each chosen by clicking the selected one again, which picks the next one under it.
- Placements are for every mode unless made for one: "Only in (mode)" makes the selected elements' placement the
  shown mode's own (a copy of the every-mode one; unticked, it's dropped), and "New placements for this mode only"
  makes new ones for the shown mode. Reset drops the shown mode's own placement if there is one, else the one for
  every mode.
- Only in 4:3 (at the 4:3 aspect): makes the selected elements' placement 4:3's own (a copy of the one in use;
  unticked, it's dropped); the other aspects keep theirs.
- Messages (the WRONG WAY, finish banners, GAME OVER) start hidden; the Messages box shows them.
- The trick name in the stunt scores' place (split screen and two stacked) isn't drawn or picked on its own: the scores' box is the
  one box, and the trick name goes with it.
- Attach to: keeps the selected elements at their offset from another element wherever that one goes (x and y are
  then the offset; dragging changes it), both ways, up and down only or sideways only (the box beside it). A dashed line joins them and the list shows what each is attached to.
  (none) detaches, leaving it placed by hand where it is; choosing an anchor or placing it by hand detaches it too.
- The server won't start on a port that's in use (no SO_REUSEADDR: on Windows it lets two servers share a port).
- Layouts: 1 player, 2 stacked, 2 side by side, 3 or 4 players (one layout).
- Select several: Ctrl/Shift-click (in the picture or the list), or drag a rectangle from empty space (Ctrl/Shift
  adds); Ctrl+A selects all, Esc none. Dragging any of them moves them all, snapping as one; anchor, scale, hide and
  reset apply to all. Moved together, those without an anchor of their own take the one their group lands on, so
  they stay together at every aspect ratio.
- Anchor: auto (by the screen third, or the third of its half split, where it's dropped) or a fixed one (left edge,
  middle of the left half, center, middle of the right half, right edge, the 4:3 screen). x and y can be typed.
- Scale: the size the selected elements are drawn at, as a percentage of the image (the list shows each one's; in
  4 players the skull is 50, the rest the lap timer's `small_scale`, 80 in the previewer), saved as a multiple of
  its default so the game keeps it pixel-fit at its resolution; or every element of the layout by a percentage of
  its default.
- Hide: the element's checkbox in the list, the Hidden box, or H. A hidden element is drawn faded in the editor.
- Play slides (or the time slider) plays WRONG WAY's slide with its anchor blending.
- Reset element (R) or the whole layout; Save writes `src/hud_overrides.inc`. Rebuild the game and the previewer
  to use it.

Each change is solved again by the engine (the previewer, `--overrides`), so elements placed against a moved one
follow it and the issues update.

## Captures

`RUSH2_HUD_CAPTURE=<folder>` (test aid) makes the game write `<folder>/frames.jsonl` every
`RUSH2_HUD_CAPTURE_EVERY` frames (default 10) of a race: the settings, every widget (how it was built, its catalog
part, where the game drew it, shown) with the layout's place as applied, the text the HUD printed, and each widget's
image as raw RGBA in `<folder>/tex/`. `tools/hud_preview/capture.py OUT CASE...` runs named cases (`--list`), each
one launch of about 70 s through ONE RACE (or STUNT, or BATTLE) with the bonus time banner (RUSH2_TEST_BONUS_TIME)
and the finish (RUSH2_TEST_FINISH). `RUSH2_TEST_MANUAL=1010` (test aid) sets each player's TRANSMISSION (record
+0x20) so the tachometer and gear show; `RUSH2_TEST_DEATHS=1` (test aid, `src/players4.cpp`) sets the DEATHS race
option (0x800D5771) so the skull shows. The script backs up and restores `build/saves` and the settings it changes.

A capture verifies the catalog once; it isn't needed to see a layout. Redo one only after changing the catalog's
reading of a callback or the game's widgets (a players4 change).

## Before the catalog

Until 2026-10-16 `src/hud.cpp` read every widget each frame (its role tagged by `src/players4.cpp`, its bounds
trimmed to its opaque texels, what had been seen of it) and laid out from that, and the previewer worked from
captures of it. An element the capture hadn't seen (the IT badge, the skull, a banner) wasn't known until a run
showed it, so elements could be placed over ones that weren't on the screen yet. Before that, each role was placed
by its own function (`place_role`) in several passes from what was shown that frame.
