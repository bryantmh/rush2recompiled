# Original game fixes

Bugs in the original Rush 2 (US) that the recomp fixes, always on (src/game_fixes.cpp). Each is a hook into the
game's own code.

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
