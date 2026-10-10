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
textures, so its class can't change. At boot (`rush2_heap_init`, src/assets.cpp) `rush2::fix_rocket_windshield`
replaces the asset with a copy whose texture 6 uses entry 190 for those texels.

## Keep music playing when paused

Option (Sound tab, off by default; src/music.cpp). `race_pause_menu_step_800AFD44` stops the music when the race
pauses (music command 0x40000000 queued at 0x800AFE40, after `func_80062FC4` at 0x800AFE38) and on CONTINUE restarts
the race's song from the top with `func_8008C370(2, MUSIC setting)` at 0x800B00B8, which picks a new song under a
shuffle. While the option is on, hooks around both calls (0x800AFE38-0x800AFE48 and 0x800B00B8-0x800B00C0) hold their
music commands: the stop becomes a volume update (0xC0000000) and the song choice does nothing, so the song plays on.
The option can be changed with the pause menu open: the hook at the pause step's entry (0x800AFD44) starts the race's
song at once (`func_8008C370`, as CONTINUE does) or stops it (command 0x40000000), and CONTINUE holds its restart
only while the song plays.
