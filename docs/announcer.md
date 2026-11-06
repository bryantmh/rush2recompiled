# Race start announcer

The Sound tab's **Announcer** (Sound > General, `race_announcer` in sound.json: `Rush2` default,
`CalSpeed`, shown as "Cal. Speed") picks which game's voice calls a race's countdown and GO. Port: `src/announcer.cpp`; hooks in us.toml next
to the `menu_play_sound_80064908` hook. Hot-swappable (read at each cue). Without California Speed's ROM, Rush 2's
sound plays. A `Rush2049` choice (removed: Rush 2049's voice is Rush 2's, below) loads as Rush 2.

Tags: **[V]** verified in disassembly or data; **[I]** inferred.

## How each game calls the start

Rush 2, SF Rush and Rush 2049 share the countdown code: a 3D sign (CNTDWN3 ... textures) per view, and a sound when
the countdown digit drops, played only for the last view. California Speed has no CNTDWN sign; its countdown function
keeps the same digit-drop test and sound calls.

| Game | Countdown | 3 | 2 | 1 | GO (race starts) |
|---|---|---|---|---|---|
| Rush 2 | `race_countdown_update_80088DF0`, digit 0x8010C014 = trunc(time left + 0.999) of 3.5 s (`race_start_timer_800AE670`) | 0x71 | 0x72 | 0x69 | 0x6C, `race_start_timer_800AE670` at 0x800AEB54 |
| SF Rush | func_80097618 (digit 0x800EB910), sounds via func_8006CABC | 0x41 | 0x42 | 0x39 | 0x3C (func_800B9F8C at 0x800BA378) |
| Rush 2049 (N64) | func_800B61FC (digit 0x80152032), sounds via func_80092360(id, 0, 1, 0) | 0x4B | 0x4C | 0x4D | 0x4E (func_800FBF88 at 0x800FC214, through func_800B61A8, before the music) |
| Rush 2049 (Dreamcast) | 0x8C022654 (the CNTDWN%d user), sounds via 0x8C011606 | 0x58 | 0x5A | 0x4E | 0x54 (0x8C027D9C, same arguments) [I: by call site] |
| California Speed | func_800ADB00 from func_800ADC20, digit 0x800FA5A8 of a 2.5 s countdown (0x80093158), sounds via func_80056268 | none | 0x5B | 0x5C | 0x5D (digit 0, as the race starts) |

[V] for all but the disc's GO.

- The digits are trunc(time left + 0.999): Rush 2's 3.5 s runs 4 (silent), 3, 2, 1 a second apart from 0.5 s, then the
  race starts at 3.5 s. California Speed's 2.5 s runs 3 (silent), 2, 1, then 0 at 2.5 s, which is its start. So the
  port plays California Speed's on Rush 2's 2, 1 and GO, and nothing on 3. Its three are "On your mark" (0x5B),
  "Get set" (0x5C) and "Go" (0x5D), a second apart, the last as the race starts, as in California Speed.
- **SF Rush is Rush 2**: SF Rush's 0x41 / 0x42 / 0x39 / 0x3C are byte-identical samples to Rush 2's 0x71 / 0x72 / 0x69 /
  0x6C (same lengths and ADPCM data), and its per-sound pitch table (0x800C3BB0) gives the same pitches as Rush 2's
  (0x800BCBD4: 0.5, 0.625, 0.625, 0.625). So the setting has no SF Rush choice.
- **Rush 2049 (N64) reuses Rush 2's recordings** [I: from recordings]: its 0x4B-0x4E played in a race
  (tmp run, rush2049 stream) have the same lengths, 50 ms loudness shapes (GO's second rise at 0.5 s included) and
  pitch (within 10%) as Rush 2's 0x71 / 0x72 / 0x69 / 0x6C decoded at their play pitches; only the encoding differs
  (2049's own ADPCM). So the setting has no Rush 2049 choice (it had one at first). The Dreamcast voice has the same shapes
  too; its pitch couldn't be measured over the 2049 engine sound in the same stream.
- Both Rush 2 and SF Rush also pick a random pair of phrases at GO (Rush 2 0x800CB570 / 0x800CB578, SF Rush 0x800D13DC
  / 0x800D13E4, the second 4 s later). Their ids are 0x80A1-0x80A6, and menu_play_sound drops ids with 0x8000 set (but
  0x8000 itself and the 0x8041/0x8047 random picks), so on the N64 they never sound; they are left alone.
- Rush 2049's countdown sounds are gated by its sound flag byte 0x8010FFC0 (1 in the ROM).
- California Speed's effect start (0x80055744): pitch from its table 0x80057D0C [id x 3] (0.5 for 0x5B-0x5D: its
  44.1 kHz samples at the 22.05 kHz output), voice volume = option / 40 x 32760 (0x800553E0), pan centered.

## Port

- Hooks before Rush 2's four calls (0x8008907C, 0x80089098, 0x800890A8 and 0x800AEB54) mark the cue
  (`rush2_announcer_cue`); the hook at the start of menu_play_sound (`rush2_announcer_sound`) then plays the chosen
  announcer's sound instead, if the call is the cue's sound and 0x800D5700 (sounds on) is set.
- California Speed: `car_engines::play_calspeed_sound` decodes the sound from its bank (as its engine sounds are) and
  mixes it once on the host at Rush 2's effects volume.

Test: pick the announcer on the Sound tab and start a race (`tools/test_race.py`); the countdown is the first 3.5 s.
