# Rush 2049 audio: MusyX data, songs, volumes and track object sounds

Tags: **[V]** verified in disassembly or data; **[I]** inferred. Addresses are 2049 main (`tools/rush2049/out/d49m.asm`)
unless marked "boot" (ROM 0x1000 → 0x80000400; disassemble with `tools/rush2049/dis49.py`, which also writes main).

Port: `include/audio2049.h`, `src/rush2049/audio2049.cpp` (namespace `rush2::audio2049`). Test:
`tools/rush2049/cpp_test/audio_build.bat <out_dir> --races [--allsfx]` renders the six race songs (60 s each and 10 s
around each loop point), checks levels, note counts against the song data, tempo and the loops, and plays the wing
sound, two more effects, a fade-out and the emitter law.

---------------------------------------------------------------------------------------------------------------------

## 1. Sound system

Rush 2049 runs Factor 5's **MusyX** (an N64 build of the 1.x runtime, with its own RSP microcode, "N64 RSP mixer
V1.1" at boot 0x8002D890) on an audio thread. It is not libaudio. [V]

| Item | Value |
|---|---|
| Init | `sndInit(22050 Hz, 32 voices, 16 music, 16 sfx)` at 0x800A5F94 (boot func_800108E0) [V] |
| Output rate | 22050 Hz; RSP frames of 192 samples (8.7 ms) [V] |
| Synth voices | 32 × 0x1A0 at boot 0x8004BEB8 [V] |
| Groups pushed | song groups 0-11 by song, sfx groups 12-15 from 0x8011F0A0 [V] |
| Volume faders | 32 × 0x28 at boot 0x8004F300: +0x0 volume, +0x18 pause volume (16.16 of 0..127) [V] |
| Sound command queue | 0x801427A8 (free list 0x80142728); audio thread switch at 0x80099390, jump table 0x80123A8C [V] |

Audio thread messages (byte +2 = type) [V for the listed ones]: 0 play song (+4 id; ignored while a song plays),
1 stop song, 4 update a sound (+4 volume, +8 pan, +0xC surround, +0x10 pitch; −2.0 = unchanged), 10 master volume
(+4 volume 0-1, +8 fade seconds, +0xC music flag, +0xD sfx flag), 11 output mode (stereo/mono flag, boot 0x8004F2F8).
The others start, stop and pause sounds.

## 2. ROM files

| File | Role |
|---|---|
| 6 | Project (LZ, 6856 B): song groups 0-11 (pages, MIDI setups), sfx groups 12-15 (119 effects 0x00-0x76) [V] |
| 7 | Pool (LZ, 12536 B): 201 macros, 5 tables (all linear ADSRs), no keymaps, no layers [V] |
| 8 | Sample directory (LZ, 195 × 0x1C) [V] |
| 9 | Sample data, raw at ROM 0x39B40, 0x2D2A30 bytes (the game hard-codes 0x39B40 at 0x800A5F20) [V] |
| 10-21 | Songs 0-11 (LZ, MusyX song format), loaded on demand by `func_80097798(song + 10)` [V] |

Formats (details in the `src/rush2049/audio2049.cpp` header):
- Project groups `{u32 size, u16 id, u16 type, u32 offsets}` relative to group + 8. Pages are 8 bytes
  `{u16 macro, u8 priority, u8 max voices, u8 ff, u8 program, u16}`; nearly every program has max voices 1 (one voice
  per instrument; a new note steals the old one). No drum pages: MIDI channel 9 is silent (only song 4 has 8 notes
  there). Sfx entries are 12 bytes `{u16 id, u16 macro, u8 prio, u8 max voices, u8 volume, u8 pan, u8 key, u8 volume
  group, u16}`: all are priority 255, volume 127, pan 64, key 60, group 0.
- Macros: 8-byte steps, opcode = low 7 bits of the first big-endian word. Opcodes used by 2049: End, WaitMs, SetAdsr,
  ScaleVolume, Envelope, StartSample, StopSample, KeyOff, FadeIn, RndNote, AddNote, SetNote, Vibrato,
  Add/SetAgeCount, AgeCntSpeed, VolSelect. Envelope curve ids name tables that don't exist, so envelopes are linear.
- ADSR tables (little-endian u16 attack ms, decay ms, sustain, release ms): c001 (0, 5548, 0, 1000), c002 (0, 420, 0,
  1195), c003 (0, 716, 0, 883), c004 (1262, 2556, 0, 1000), c005 (0, 1574, 1765, 1000).
- Sample directory `{u16 id, u16, u32 offset, u32, u8 root key (always 60), u8, u16 rate, u32 format << 24 | length,
  u32 loop start, u32 loop length}`. All 195 samples are format 3 (2049 ADPCM, see `src/rush2049/wings_sound.cpp`); rates 4181
  to 39069 Hz, mostly 11025 / 22050 / 16726.
- Songs: header `{track table (0x18), pattern table, channel map, tempo track (0 in all), bpm, loop tick}`, 64 tracks
  of 12-byte entries, patterns with 16-bit delta times (the GameCube-era "revised" format). 384 ticks per beat.

`rush2::rom2049::read_file` hangs on file 7 [V]: it probes every file with inflate first, and miniz's
`tinfl_decompress_mem_to_heap` never returns on that LZ data. The audio port reads files 6-8 and 10-21 straight as
LZ. Other LZ files may do the same; trying LZ first for files known to be LZ would avoid it.

## 3. Songs

Per-track table 0x8010FFD4, s32[19], used when the music option (0x8014610E, default 12) is 12 "per track"
(func_800D6160); −1 = random. Options 0-11 play that song everywhere. Random picks `rand() * 12 / 32768` and
re-rolls 5. [V]

| Track index | 0-5 (race 1-6) | 6-11 | 12-18 |
|---|---|---|---|
| Song | 0, 1, 4, 2, 3, 7 | 0, 1, 4, 2, 3, 7 | 8, 9, 8, 8, 9, 9, 8 |

| Song | File | bpm | Length | Loop | Race track | Disc song |
|---|---|---|---|---|---|---|
| 0 | 10 | 32 | 255.0 s | to 0 | 1 (Marina) | Bassy |
| 1 | 11 | 151 | 220.9 s | to 0 | 2 (Haight) | Garage |
| 2 | 12 | 137 | 266.3 s | to 0 | 4 (Metro) | Wingey |
| 3 | 13 | 154 | 268.1 s | to 0 | 5 (Mission; sparse intro: 108 notes in the first minute) | Trancey |
| 4 | 14 | 132 | 240.0 s | to 0 | 3 (Civic) | — |
| 5 | 15 | 134 | 96.7 s | to 7.2 s | — | — |
| 6 | 16 | 142 | 142.0 s | to 13.5 s | — | — |
| 7 | 17 | 140 | 246.9 s | to 0 | 6 (Presidio) | — |
| 8 | 18 | 103 | 242.3 s | to 0 | — | — |
| 9 | 19 | 130 | 265.8 s | to 0 | — | Flier |
| 10 | 20 | 140 | ends (~34 s) | — | — | — |
| 11 | 21 | 122 | ends (~32 s) | — | — | — |

Disc song: the Dreamcast `.STR` that the N64 song arranges. Matched by tempo and by time-aligned chroma: 60 s of the
N64 render (`audio_build.bat --song N`) slid over the whole decoded stream, mean frame correlation 0.54-0.87 for the
match against 0.26-0.43 for every other stream. Songs 4, 7 and 8 have no clear counterpart (best 0.36, 0.48, 0.49 with
runners-up within 0.03; 8 sits closest to Noon and Sunset). The Sound tab (src/music.cpp) names the matched N64 songs
after their disc song and keeps a number for the rest. [I]

Song n is played with `sndSeqPlay(group n, song n, data, NULL)`: no start volume or fade, all tracks on, speed 1
(func_800979A0; group/song pair table 0x8011F070). A new song only starts once the old one was stopped. [V]

Front-end songs (main menu music option, src/music.cpp):
- Rush 2: the front end (game state 0x8010C0D0 = 0, func_800AD07C and func_800ABE7C) sends music command 0x0009FFFF = sequence 9
  (not a race song). Other direct plays: 0xC at state 6, 5/6 in the menu loop by byte 0x80125B11. [V]
- Rush 2049: song 5 and 6 are not race songs. `func_800C9194(song, 1)` (play) is called with 6 from the menu loop
  func_800D71D0 and func_800CA3B4, with 5 from func_800F5F90. Song 6 is taken as the main menu's. [inferred]
- SF Rush: the music command function is func_8006CA08 (Rush 2's func_80062F50). Screen 0 of the front-end dispatcher
  (func_800B9978 jump table 0x800D7F3C -> func_800B9508) plays sequence 11 (command 0x000BFFFF). Other plays: 4 (B9978),
  13 (B4F94), 14 (B0374, state 6), 5 (B449C). Screen 0 is taken as the main menu. [inferred]

## 4. Volume calibration

Master volumes: `sndMasterVolume((u8)(option / 10 * 127 * 0.9))` with the music option (0x8014610C) and the
sound-effects option (0x8014610D) (func_800D6160, func_800D6530 → message 10 → func_80095528 → boot
func_80020494). Both default to 10 (func_800CCB40, settings 12 and 13), so both master faders are **114/127**; the
change fades over 1 s (0.05 s from some menus). The music option 0 gives master 0. [V]

Voice gain, N64 integer chain (boot 0x8001ADA8), then boot func_8001E0E0 [V]:
```
v = voice volume (velocity << 16, scaled by envelopes; 0..0x7F0000)
v = pauseVol(group) * (v >> 7);  v = volume(group) * (v >> 7)     // 127 and 127
v = master * (v >> 7)                                            // 114 (music fader 21, sfx fader 22)
if (track) v = trackVolume * (v >> 7)                            // 127, music only
v = (MIDI volume CC7) * (v >> 7)                                 // no CC11 on N64
f = vol_tab[v >> 16] lerp (129 entries, boot 0x8002CA40)
pan = 0x400000 + (CC10 - 64) << 16, span = CC131 << 16
front = f * pan_tab((0x800000 - span) / 0x400000),  S = f * pan_tab(span / 0x400000) * 0.7079
L = front * pan_tab((0x800000 - pan) / 0x400000) * 32767,  R = front * pan_tab(pan / 0x400000) * 32767
pan_tab = {0, 0.7079, 1, 1}
```
Check: the wing sound as 2049 plays it (sfx 0x3D, volume 0.5 → CC7 63, pan 0 → CC10 63, pitch 0.75) comes out of
the port at voice gains **3568 / 3490** and step 1535/4096, exactly the values `src/rush2049/wings_sound.cpp` took from the
running game.

Output scale: the RSP mixer isn't ported. With a 15-bit gain of 32767 = 1.0 the race songs would peak up to
+6.7 dBFS (song 7) and run at −11 to −16.5 dBFS RMS, which a composer on the N64 would have heard clip, so the mixer
evidently keeps 6 dB of headroom; `audio2049` uses **0.5**. `src/rush2049/wings_sound.cpp` uses 3568/32768 (1.0 scale), so it
is 6 dB louder than `audio2049`'s sfx 0x3D at the same settings.

Port output at music_gain = sfx_gain = 1 (48 kHz, first 60 s):

| Song (race track) | Peak | RMS | 1 s RMS range |
|---|---|---|---|
| 0 (1) | −4.4 dBFS | −19.3 dBFS | −29.2..−17.3 |
| 1 (2) | −5.1 | −22.5 | −26.6..−18.4 |
| 4 (3) | −2.2 | −17.1 | −20.4..−15.2 |
| 2 (4) | −3.6 | −20.4 | −28.7..−17.0 |
| 3 (5) | −4.3 | −20.9 | −39.7..−17.0 |
| 7 (6) | +0.7 (25 samples over 1.0) | −17.4 | −22.8..−14.7 |

Effects at volume 1, pan 0: peaks −11 to −13 dBFS for almost all (the samples are normalised); full table from
`--allsfx`.

## 5. Sound effects as Rush 2049 plays them

Start (func_80097AFC): `sndFXStartEx(id, 0xFF, 0xFF)` (default volume and pan), then MIDI volume `(u8)(volume * 127)`
and pan `(u8)((pan + 1) * 0.5 * 127)`. Updates (func_80097CA0, message 4): volume → CC7, pan → CC10, surround → CC131
`(u8)((s + 1) * 0.5 * 127)`, pitch → Doppler CC132/133 `(u16)max(pitch * 8192 - 1, 0)` (so pitch ≤ 2). Stop: key-off
(boot func_8001FE58) or kill. Floats convert to integers by truncation. [V]

`rush2::audio2049::sfx_start / sfx_update / sfx_stop` take exactly these parameters.

## 6. Track object sounds (movers and props)

Type table 0x80117530 (122 × 0x30): +0x1C start sound, +0x20 loop sound, +0x24 stop sound (−1 none), +0x28 flags,
+0x2C range. Types with sounds [V data]:

| Types | Start | Loop | Stop | Range |
|---|---|---|---|---|
| BKWBARIER, BLOCK1FOR/BAC, BLOCK2FOR/BAC, GREGDOOR1/2/2B/3/5/6, GREGRAMP, RAMP, RAMP02, T3DOOR1, T3PYRAMID, T3TEETER | 0x3F | 0x3D | 0x3E | 400 |
| BLOCK, BLOCKNV | 0x31 | 0x3D | 0x3E | 400 |
| GREGDOOR4 | — | 0x3D | — | 400 |
| BARGE | — | 0x61 | — | 400 |
| F1FLAG | — | 0x12 | — | 400 |
| GONDOLA1-4, GONDOLAN1 | — | 0x49 | — | 400 |
| MINITRAIN, MINITRAINN | — | 0x01 | — | 400 |
| TRAINORG, TRAINNIGHT | — | 0x01 | — | 800 |
| TROLLEY, TROLLEYNT, TROLLEY2 | — | 0x04 | — | 400 |
| PLANE | — | 0x0E (flags 2) | — | 2000 |
| WINDROTOR1 | — | 0x0A | — | 400 |
| TRIGGER | 0x42 | — | — | 400 |
| GLASSWALL, TOILET (path objects) | 0x0F | — | — | 400 |
| Props: BUMPHIT, CURVEHIT, METER, MPH45HIT, MPH75HIT, NOPARK, SLOWHIT, STOPHIT, THINKHIT | 0x11 | — | — | 400 |
| CONE1, GASPUMP | 0x08 | — | — | 400 |
| RAT, RATCONE, YIELDHIT | 0x2F | — | — | 400 |
| FENCE, GETOFF | 0x0D | — | — | 400 |
| CACTUS | 0x17 | — | — | 400 |
| SHATPANE | 0x0F | — | — | 400 |
| BULB, GUARDRAIL, GOLDCOIN, SILVERCOIN | 0x06 | — | — | 400 |

(WEPICON_* 0x60 and WPR_MINE 0x45 are battle-only.) The prop "start" sounds are presumably played on impact by the
knock-over code [I]; the path-object state machine is movers.md §4.5.

Path-object state machine (func_800BF394 start, func_800BF1C8 stop, func_800BF45C moving; state obj+0x64, handle
obj+0x60) [V]: start = stop the current sound, play +0x1C, state 0. Moving: state 2 → start; state 0 and the start
sound has finished → play +0x20 (loop), state 1; state 1 → update the loop's emitter each frame. Stop: if state ≠ 2
and the start sound isn't still playing, stop the current sound, play +0x24, state 2.

Each sound is a 3D emitter (func_800AED64(pos = obj+0x38, vel = zero vector 0x801141B0, range = type+0x2C,
max volume = 1.0, min volume = 0.0, id, 0, flags = type+0x28, priority 0x80)) [V]. Loop update (func_800BF2B8: emitter
max volume, pitch) [V]:
- loop sound 0x12 (F1FLAG): max volume 0.8, pitch 1.0;
- 0x61 (BARGE): max volume 1.0, pitch 0.75;
- 0x01 (trains): f = clamp(|(int)speed| / |(int)max speed| * 0.5 + 0.5, 0, 1) from the follower (obj+0x6C +0x14 /
  +0x10), max volume 0.75 f + 0.25, pitch f;
- others: max volume 1.0, pitch 1.0.

Emitter law (func_80098AE4, every audio frame, per listener L) [V], implemented as `audio2049::emitter_mix`:
```
d = emitter - L.pos;  dist = |d|;  d /= dist (d = (1,0,0) if dist = 0)
g = range > 0 ? max(0, maxVol - (maxVol - minVol) * dist / range) : maxVol
right = L.up x L.back                     // C = B x A, A = listener +0x24, B = listener +0x30
pan_i = (right . d) * (1 - g);  sur_i = (L.back . d) * (1 - g)
volume = clamp(sum g_i, 0, 1);  pan = sum pan_i * g_i / sum g;  surround = sum sur_i * g_i / sum g (0 if sum g = 0)
→ sound update (volume, pan, surround, pitch = emitter pitch)
```
Listeners (func_800DEC8C, per player from 0x8014A118 + 0x4C i, listener +0x44): position = the player's car (+0x22C),
A (+0x24) = camera i +0x18 (row 2, pointing back), B (+0x30) = camera i +0xC (row 1, up); cameras at 0x80150B70 +
0x98 i [V addresses, I row meaning]. A sound behind the camera gets surround +1, which moves it into the Dolby
Surround channel (front gain ×0.0156, surround ×0.7079), i.e. in stereo it plays as left + S, right − S. Volume falls
off linearly to 0 at the range (400, 800 or 2000 units). Rush 2049 also ranks its sounds (func_80098710: the start byte
0x80 + (1 − volume) × 80, +8 when the id's byte in 0x8011F5CC is set) to limit how many play at once [V code,
I purpose].

## 7. Other track-tied sounds

- No ambient sound emitters in 2049 placements (placement.md §4); Rush 2's ambient types don't exist in 2049 data.
- Surface type 3 (deadly water, track 5) plays sound 0x15 when a car is wrecked in it (collision.md §5). [V]
- Coins (0x06) and the prop and mover sounds above are the only object sounds.
- No reverb: every MIDI setup has reverb and chorus 0, and no macro sets a send.

## 8. What the port leaves out

Keymaps, layers, PlayMacro, portamento (CC65 is never set), traps/messages, variables, LFOs, tremolo, pitch ADSR, DLS
ADSRs, curve tables, reverb/chorus, song sections and the RSP mixer itself (output scale 0.5, see §4). Pitch and volume
updates follow MusyX's job timing (pitch every 15 ms, volume every 5 ms, ramped).

## 7. Engine sounds

Port: `src/rush2049/engine2049.cpp`. [V] unless marked.

Rush 2049 picks a car's engine sound by its **ENGINE setting** (physics car +0xC, 0-5), not by the car: func_800D5E64
(race start) points the car's engine state (0x80140420 + car * 0x54: +0 table, then two 0x14-byte layers {+0 handle,
+4 last volume, +8 last pitch}) at 0x8010FD80 + ENGINE * 0x40 and starts each layer's sound: the local player's
car (+0x7CC == 2) without position (func_800D5C90), other cars as 3D emitters (func_800AED64: range 400, volume
0-1, priority 0x80). A layer is 0x20 bytes {s32 sound (-1 none), u16 base rpm, u16 span, u16 rpm points [3], u16,
f32 volumes [3], f32}:

| ENGINE | Layer 0: sound, base / span, points, volumes | Layer 1 |
|---|---|---|
| 1 | 0x66, 2900 / 4000, 3000 4500 6000, 1.0 0.8 0.0 | 0x73, 5000 / 5000, 3000 4500 6000, 0.25 1.0 1.0 |
| 2 | 0x69, 4000 / 4000, 3000 3000 6000, 1.0 1.0 0.0 | 0x6A, 5000 / 5000, 2000 3500 6000, 0.25 1.0 1.0 |
| 3 | 0x6D, 3000 / 3000, 3000 4500 6000, 1.0 1.0 0.0 | 0x6C, 5000 / 5000, 2000 5000 5000, 0.25 1.0 1.0 |
| 4 | 0x71, 2500 / 3000, 3000 3000 6000, 1.0 1.0 0.0 | 0x72, 5000 / 5000, 2000 5000 5000, 0.25 1.0 1.0 |
| 5 | 0x6E, 5000 / 5000, 1500 3000 5000, 1.0 1.0 1.0 | none |
| 6 | 0x74, 2500 / 3000, 2000 3000 6000, 0.5 0.75 0.0 | 0x6B, 5000 / 5000, 900 3000 5000, 1.0 1.0 1.0 |

(Entries 6 and 7, sounds 0x68 and 0x70 / 0x6F, aren't reachable from the setup.)

func_800E0050 (from func_800E05F0, every other frame) updates each layer from rpm = |car +0x7D0| (engine rad/s
(+0x408) x 9.549 x 0.9, func_800D03AC):
- pitch = 1 + (rpm - base) / span, clamped to 0-2;
- volume = v0 below point 0, linear v0 -> v1 -> v2 up to point 2, v2 above; times the load factor
  0.85 + (+0x404 + 200) / 900 x 0.15 (+0x404 is the engine torque);
- the local player's car: volume x (0.8 - 0.05 x players), pitch and volume sent only when they change;
- other cars: volume x 0.75 as the emitter's maximum volume (func_800BF2B8), at the car's position (+0x22C).

Outside races (0x801174B4 without 0x400000) other cars' rpm is held at 890, and they also play a random sound
0x62-0x64 (1 in 5 per update, range 400, volume 0.8) [I: the race bit's meaning].

Rush 2 keeps the same rpm at its car +0x7F0 (func_80069254, the same 9.549 x 0.9) and the torque at +0x3F4, and plays
engines only for the local players (src/engine_preview.cpp has its slot mechanism), so the port plays 2049's engine
for a local player driving a 2049 car, in place of that slot's Rush 2 engine, as 2049 plays its own car.

**Other cars (port, `src/car_engines.cpp`, Sound tab option "Other Cars' Engines", on by default).** Rush 2 has
engine voices only for the two local players (func_80062CC4 loops over slots 0-1; its track emitters, func_80099C20 /
func_80064D24, are at most 20 fixed points, two sounding at once, volume (r^2 - d^2) / r^2 without pan), so its
computer cars are silent. The port gives every car not driven by a local player its engine with Rush 2049's emitter
law (range 400, 0.75 of the engine volume, split screen summed over the local players). A 2049 car plays 2049's
engine through `audio2049` (2049's drone setup has ENGINE 1 for every car: row C at 0x80111080 is all zero). A Rush 2
car plays Rush 2's engine for its type's default sound with Rush 2's law: sound 0x800BD1FC [engine * 2 + layer],
pitch min(r / 0x800BD15C [same], 2), volume (int)((int)(A[layer][min(r / 1000, 10)] x B[layer][clamp((l + 80) x 0.025,
0, 12)] x 2000 + 24000) x 0x800D577A / 40) with A = 0x800BD338, B = 0x800BD29C, r = |car +0x7F0| x [0x800E7BA4] and
l = car +0x3F4 x [0x800E7BAC] (as u16 & 0x7FFF, func_800650DC). Its samples (VADPCM, the ALSound list of the
sound effect ALInstrument at 0x800D2478; the engine sounds have key base 40, no detune, full volume and sustain) are
decoded from the ROM and mixed on the host at pitch samples per output frame, as libaudio's sound player plays them.
