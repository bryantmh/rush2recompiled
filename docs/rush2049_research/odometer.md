# Rush 2049 odometer

Rush 2049 shows each player's distance driven this race on four rolling digits. The recomp ports it as the Odometer
option (Games tab, Rush 2049 section): `src/rush2049/odometer.cpp` draws it and `src/hud.cpp` places it.

## HUD callback (0x80106D94)

Addresses are Rush 2049 (N64 USA).

- Four HUD widgets per player draw the odometer: layout entries 0x80114E50-0x8011506C, image `ODOMETER`, callback
  `func_80106D94`. The parameter is `player << 4 | digit`.
- The callback reads car state +0x108 (0x80152818 + car x 0x3B8), the distance in feet.
- With the metric flag 0x80146111 set, the callback divides the distance by 0.6 to get km.
- Digits 0-3 are hundreds, tens and units of miles, then tenths. Each digit shows (distance / 528000, 52800, 5280,
  528) mod 10, as a cell 8 texels tall of the image.
- The tenths roll all the time. A whole digit rolls by the tenths' fraction while the tenths read 9 and every whole
  digit after it reads 9.
- The digits sit at x - 16, x - 8, x and x + 8 of the player's position. Table 0x80115AE8 holds {x, y} per player for
  1-4 players; with one player it is (30, 32), under the race time.

## Distance (0x800F8EC8)

`func_800F8EC8` is the checkpoint gate test, run every physics step. It adds the length of the car's move since the
last step to car state +0x108.

Rush 2 keeps no such field. The port adds up the move of each player's car once per frame instead, from the position
the car is drawn at (car state +0, 0x801124A0 + car x 0x354).

- A frame's move past 300 feet isn't counted: that is the car being put on the grid, not driving.
- The distances reset from a hook at the start of the race setup, `func_800A5110` (0x800A5114). That hook also runs on
  a restart.
- Rush 2's speedometer unit byte 0x800D4160 (nonzero: km/h) picks km.

## Image

`ODOMETER` is in Rush 2049's HUD file 64.

- It is 16 x 88 texels at 4 bits a texel with a 16-color palette. Its rows are stored bottom up.
- Upright, each column holds 0-9 and 0 again in cells of 8 x 8.
- The left column has the whole digits, white on black. The right column has the tenths, black on white.

The port splits the two columns into RGBA16 images in spare RDRAM: 0x80C9A000 for the whole digits and 0x80C9A600 for
the tenths. Each one is clamped at its own edges. A Dreamcast source's larger image is averaged down to the N64 size.

## Placement in Rush 2

- The odometer goes with each player's radar, lined up with the radar's outer edge.
- It sits above the radar. In the bottom row of quadrants, where the radar is at the top, it sits below it.
- A deaths skull that would overlap it moves to the odometer's inner side.
- Without a radar, it goes under the race time and lap box, or over them in the bottom row.
- With 3 or 4 players, the odometer, race time and place are drawn at the scale nearest 75% at which a texel covers a
  whole number of output pixels (`fitting_scale`).
- These shrunk images are point sampled, so no transparent texel color bleeds into their edges.
- RT64 rounds every rectangle edge up to a whole 4:3 pixel and stretches the texture over that. A shrunk image ending
  between pixels would run past its last texel into a repeat of its edge (a thick far border), its first column
  wrapped around, or the next glyph of a digit strip. So each shrunk image spans whole pixels and its texture step is
  set to fit exactly (`fit_span`): the first and last texel get the same width (an even outline), and every texel is
  within one output pixel of the others.
- At Original resolution (one output pixel per 4:3 pixel) no shrink keeps every texel: the race time's 7-texel glyphs
  drop strokes.
