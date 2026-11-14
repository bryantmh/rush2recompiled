# Checkpoint time: Rush 2 vs Rush 2049, and what 2049 tracks get in Rush 2

Tags: **[V]** verified in code or data; **[I]** inferred. Function addresses are Rush 2 unless marked "2049".
Scripts used for the tables are one-off; the parsers are `tools/rush2049/paths.py` and `roms.py`.

Report under investigation: "on the 2049 tracks there isn't enough time between checkpoints".

---------------------------------------------------------------------------------------------------------------------

## 0. Correction (verified in game): the race time is computed from the AI lanes

§1–§2 below are wrong about the values Rush 2 uses. `func_80093300` does write 90 / 45 into the header copy at
`0x8010BCE8`, but `func_80093048` then calls **`func_800924E4`**, which overwrites them:

- each checkpoint's extensions (+0x1E lap 1, +0x20 later laps) = the time to drive from it to the next checkpoint
  along the AI lanes, `distance / (lane point speed byte × 1.4666667)` summed per lane point (0x80092644–0x80092790);
- the start time (+0) = the first stretch's time + 10 (0x800927CC) plus a share of the rest (÷ 30.0, 0x80092818–
  0x80092850).

So the times depend on the path's lane speeds. Read in game at difficulty 2 (f = 1.225): Las Vegas forward header 32,
first checkpoint 19 / 22 (39 s at the start); Rush 2049 track 1 forward header 15, first checkpoint 4 / 5 (18 s at the
start). Rush 2049 drives faster cars and has no countdown, so its lanes are faster than Rush 2's: distance-weighted
lane 0 speed is 139.3 on average over Rush 2's 9 race paths (both directions), against 0.91–1.31 times that on the
2049 paths (track 1: 1.30 / 1.26, track 3: 1.31 / 1.12).

**Fix** (`rush2_track49_race_time` in `src/rush2049/track2049.cpp`, hook in `func_80093048` at 0x80093298): on a 2049 track
the computed start time and extensions are multiplied by the path's lane 0 speed ÷ 139.3 when that is above 1, which
is what they would be at Rush 2's lane speeds. Both speeds are in the current Car Speeds mode (cars.md §10): the 2049
lanes are already / 1.2 (true mph) with Rush 2 speeds, so only paths faster than Rush 2's average still get more time.

### 0.1 Flat lane 0 on Marina and Civic forward (2026-10-21, report: "Civic checkpoints feel really close")

`func_800924E4` times every stretch along **lane 0 only**. Rush 2's four lanes agree to within about 1 s per lap, so
the choice never mattered there. Some 2049 path files have a lane 0 at one speed everywhere, with no corner slowdown:

| path | lane 0 speeds (raw) | lap time per lane, s (Rush 2 speeds, /1.2) | granted per lap after the 139.3 scale |
|---|---|---|---|
| Marina f (158) | 181 everywhere (lanes 2, 3: 180) | 57, 70, 58, 58 | 62 (×1.08) |
| Civic f (160) | 181 / 190 | 78, 89, 90, 88 | 85 (×1.09) |
| Haight f (159) | varies (lane 3: 226/247) | 99, 98, 98, 60 | 99 |

Civic forward is granted about 5% less than its real lanes' pace, and Marina forward about 11% less. Civic backward
(179) has four equal lanes (92 s) and is fine. Presidio b lanes run 186-224 s, which still needs a look: some lanes
may hold AI stop points.

**Fix (2026-11-03, report: "Marina doesn't have long enough checkpoints"):** `path_race_time`
(`src/rush2049/track2049.cpp`) now gives each stretch the time of its **slowest** lane: it walks every lane between
the header copy's lane crossings (checkpoint +0x22 slots 1-4) at the mapped lane speeds, and each checkpoint's
extensions are multiplied by max(the 139.3 scale, slowest lane ÷ lane 0) for that stretch; the start time by the same
ratio over the whole lap. Rush 2's own paths are untouched (2049 tracks only), and where a path's lanes agree nothing
changes. The average of the lanes was rejected: Marina f's three flat lanes pull it to 61 s, below the 62 s the 139.3
scale already gave. Per lap before the difficulty factor, Rush 2 speeds:

| path | forward | backward |
|---|---|---|
| Marina | 58 → 66 (+14%) | 0 |
| Haight | 0 | 0 |
| Civic | 82 → 90 (+10%) | 0 |
| Metro | 114 → 132 (+16%) | 120 → 124 (+3%) |
| Mission | 92 → 99 (+8%) | 0 |
| Presidio | 214 → 216 (+1%) | 206 → 257 (+25%: lanes 1-3 hold a 26-28 mph stretch lane 0 skips) |

Grip was checked too: capped to 3.3 g (what Rush 2's circuit lanes ask for) with 40 ft/s² braking, 2049 lanes lose
2-5% a lap, Rush 2's circuits about 1% and its Alcatraz and LA about 6%, so the lanes' corner speeds aren't the
shortfall. Scripts: `tmp/stretch_times.py`, `tmp/lane_g.py` (one-off).

The arcade Civic (custom track `civic_arcade`, no scale, lane 0 = `AIPATH_lane1`) is granted 87 s per lap, the same
as `AIPATH_main` and `lane4`. `lane2`/`lane3` lap in 106 s only because they hold a speed-1 AI stop point. Both
Civics' real lanes average about 137-139 mph (Rush 2 mph), at the fast end of Rush 2's tracks (125-148), on a
city layout whose lanes were tuned for 2049 cars.

### 0.2 Measured: drone pace against the granted time (2026-11-05, report: "out of checkpoint time in 2nd or 3rd")

`func_800924E4` in full: pass 1 writes each checkpoint's +0x1E from the arm checkpoint round to the finish, pass 2
each +0x20 from the finish round to the finish, both as lane 0's time (point speed x 1.4666667 ft/s, truncated).
The start time is the first checkpoint's +0x1E + 10 + (the sum of every +0x20) / 30, and that lap/30 share is also
added to the arm checkpoint's +0x20. The difficulty factor f = 1 + (5 - d) x 0.075 is applied when time is added.

Sim races (`tools/test_race.py T --drones 5 --laps 2 --turbo --config cheats.cheat_no_game_timer=true --config
general.drone_catch_up=false`; drones' gate times from the samples' `next_cp`; granted times read with `--peek` on the
header copy 0x8010BCE8, without which the watcher takes no samples). Difficulty 2 (JUST PLAY's default, f 1.225):

| track | start (x f) | granted per lap | drone pace / granted | slack at drone pace (d 2) |
|---|---|---|---|---|
| Las Vegas (0) | 32 (39.2) | 116 | 1.04 slower | +18% |
| Rush 2049 Marina (12) | 20 (24.5) | 68 (after §0.1) | 0.88 (faster) | +39% |
| Marina Arcade (custom) | 23 (28.2) | 59 | 1.06 slower | +16% |
| Cal Speed 64 (point to point) | 64 (78.4) | first stretch 45 | about 1.05 slower | about +17% |

So the arcade lanes, after the converter's / 1.2, fit Rush 2 cars about as well as Rush 2's own lanes. At drone pace
the slack is f / 1.06 - 1 on Marina Arcade: d 3 +8%, d 4 +1%, d 5 -6% (and -4% on Las Vegas). Cal Speed's large
start is its long first stretch, not looser timing. With Drone Cheating on, drones ahead ease off toward the player,
so being 2nd or 3rd doesn't mean the player is at the drones' pace.

Presidio Arcade forward: all four arcade AI tables ran down from about 45 mph to 1-2 mph over lane points 330-430,
then climbed back like a car accelerating from a stop. That was the arcade's grid: its lap is a run-in the converter
had closed with a 6,000 ft straight (granted 183 s for the first stretch, and the drones turned about and wrecked at
the junction). Fixed by converting it as a run-in circuit, and Civic Arcade's standing-start lane speeds (12-37 mph at
points 1-3) lifted; see docs/arcade2049.md#dpathrom-the-drones-paths.

### 0.9 One rule: the race time model

(2026-11-05, supersedes §0's 139.3 scale and §0.1's slowest-lane rule, both removed.) Rush 2's rule grants lane 0's
drive time at its own speeds, which on Rush 2's 14 paths is 0.978 (0.90-1.07) of the best lap Rush 2's average car
can drive on lane 0 (tools/calspeed/ai_pace.py's model: STANDARD torque curve, the 21 cars' mean mass 108.36 slugs,
torque scales 1.820 / 1.842 / 1.885, gears 3.095 / 1.754 / 1.254 / 0.956, final drive 3.3, 2.6 g on the lane's
60 ft curve, 40 ft/s^2 braking). Other games' lanes give 0.78-1.15 (Rush 2049 / 1.2: 0.86-1.13; arcade: 0.87-1.15;
California Speed / 1.2: 0.78-1.14). So every Rush 2049, California Speed and custom race track (not Rush 2's, not SF
Rush's, not a custom track with its own `checkpoint_time`) is timed by `src/race_time_model.cpp`: each stretch gets
0.978 x the model's time for it (/ 1.2 with Rush 2049 car speeds), then Rush 2's start rule. A lane whose ends are
over 1,000 ft apart (run-in, point to point) starts from a standstill.

func_800924E4's start rule in full: start = checkpoint 0's lap-1 time + 10 + (the loop's later-lap times summed) /
30, and the arm checkpoint (header +6) also gets that 30th on its later-lap time; each stretch is truncated.

Built with each track's conversion (`rush2::race_time::fill`, called by track2049.cpp, calspeed_convert.cpp and
track_mod.cpp before `save_2049`) into `ConvertedTrack::lane_time` (cumulative seconds per lane 0 point, both
directions), which the disk cache keeps (format 4): the launch's build pass makes it, a race only reads it
(`rush2::race_time::apply` at 0x80093298). Log line per race: `[racetime] model lap ... start ..., checkpoints ...`.
C++ model lap on Rush 2049 Marina forward 64.6 s against the Python model's 64.55.

Sims (5 drones, catch-up off, difficulty 2), seconds before the 1.225 factor:

| track | start before -> after | lap before -> after | drones' lap / stretches |
|---|---|---|---|
| Las Vegas (Rush 2, unchanged) | 32 | 116 | 0.96 of its grant |
| Rush 2049 Marina | 20 -> 17 | 68 -> 60 | about 60 |
| Marina Arcade | 23 -> 26 | 59 -> 61 | about 62.5 |
| Rush 2049 Presidio | 38 -> 42 | 143 -> 126 (loop) | CP1-CP3 64 s (granted 68) |
| Presidio Arcade | 42 -> 42 | 123 -> 127 (loop) | CP1-CP2 28-33 s (granted 32) |
| Cal Speed track 1 (point to point) | 64 -> 56 | race 186 -> 181 | CP1-CP2 30.5-33.5 s (granted 34) |

Known limit (accepted): the model's corners are flat at 2.6 g. On Rush 2049 Presidio's CP1-CP2 (flat in height) the
drones corner at 4.5 g at the 90th percentile (2.2-2.5 g median), the Rush 2 car among them as much as the 2049 cars,
so banking or wall contact lets them through corners the model takes at 84-102 mph at 130; the stretch is granted
about 15% more than the drones need. Generous rather than tight, so left as is; reading bank angles from the
collision at `fill` time would tighten it.

## 1. Summary (superseded by §0: the 90 / 45 values below are overwritten from the AI lanes; they are NOT the race times)

- **Rush 2 has no per-track checkpoint time.** Every race on every track, a 2049 track in the host slot included,
  gets the same time:
  - start: 90 × f
  - each checkpoint: + 45 × f
  - where f = 1 + (5 − difficulty) × 0.075
  
  The values in the path file are overwritten at load and never read. **[V]**
- **Rush 2049 has no race timer at all.**
  - Its race options have no CHECKPOINTS entry.
  - Its checkpoint handler adds no time.
  - Its path loader also forces 90/45 into the header copy, but nothing reads them.
  
  So 2049 has no per-checkpoint times for Rush 2 to copy. **[V]**
- **The per-checkpoint "time" fields in the path files (both games) are editor estimates of driving time.** They
  equal the time to drive each checkpoint segment at the AI lane's target speeds, to within about 1 s.
  - Used as extensions they would leave no slack, so they would give *less* time than now.
  - For example, 2049 race 1 forward would get 7–12 s per checkpoint instead of 45 s.
  
  **[V]** (table in §5)
- **The current 2049 values aren't short.** Measured against the AI-lane driving time, the 2049 tracks get as much
  slack as Rush 2's own tracks, and usually more:
  - per lap: budget ÷ lane lap time = 1.4–5.6 for 2049, against 1.3–4.1 for Rush 2
  - for the longest checkpoint segment (§5)
- **Checkpoints register on 2049 paths.**
  - The gate test is the same in both games.
  - Simulated along every AI lane of all 12 2049 paths, it registers every checkpoint, every lap. **[V]**
- **No time value was changed.** No table, file field or constant explains a shortfall, so changing them would only
  guess. §7 lists what to check in game and an optional hook if 2049 races should be untimed, the way Rush 2049
  plays them.

## 2. Rush 2 timing (superseded by §0 for the values) [V]

| Step | Code | What it does |
|---|---|---|
| Path load | `func_80093300` (from `func_800A37F4` 0x800A37DC, `func_800A5ADC` 0x800A579C) | copies the header (0x32C bytes) of path asset `0x57+t` / `0x63+t` to `0x8010BCE8`, then **writes header+0 = 90 (0x5A, 0x80093338) and, for each checkpoint, +0x20 = 45 (0x2D, 0x80093354) and +0x1E = +0x20**. The file's values are lost. |
| Race start, checkpoints ON (`0x8010C17B` = options+0x19 ≠ 0) | `func_800AE670` 0x800AEAB8–0x800AEB28 | `0x8010C204` (time allowed) = header+0 × f; `0x8010C034` = clock `0x80117488` |
| Race start, checkpoints OFF | `func_800AB1B4` | the whole race's time up front: 90 × f + Σ over every checkpoint of the race (laps × count, up to the last finish) of its bonus × f. That is the same total. |
| Checkpoint passed | `func_800A1468` (per car of type 0/8: sign change of (pos − gate) · dir **and** x/z distance² < gate radius², for the car's *next* checkpoint only) → `func_8008F220` | for the human (type 8), not in modes 2/3 or demo, when the player's count `0x801124A0[p]+0x345` passes the global `0x8010C038`, and not on the last finish: `0x8010C204 += bonus(cp) × f`, bonus = +0x1E on lap 1, else +0x20 (both 45) |
| Respawn | `func_80090A40` | if the reset point is past the next checkpoint's crossing on the spine/lane/branch, it calls `func_8008F220` too |
| Time left | `0x8010C204 − (0x80117488 − 0x8010C034)`; out of time → state 9 | |

- f constant: 0.075 at `0x800D0060` (start), `0x800CFBEC` (checkpoint) and `0x800CFF64` (`func_800AB1B4`).
- Difficulty: byte `0x8010C211` (options+0xA, 0–5).

| difficulty | 0 | 1 | 2 | 3 | 4 | 5 |
|---|---|---|---|---|---|---|
| f | 1.375 | 1.30 | 1.225 | 1.15 | 1.075 | 1.0 |
| start (s) | 123.75 | 117 | 110.25 | 103.5 | 96.75 | 90 |
| per checkpoint (s) | 61.875 | 58.5 | 55.125 | 51.75 | 48.375 | 45 |

- Laps, backward, mirror and the track don't change the values.
  - Laps only change how many checkpoints the race has.
  - Backward only changes which path file is loaded (same 90/45).
- The recompiled code (`RecompiledFuncs/funcs_16.c`, `func_80093300`) has the same 0x5A / 0x2D constants.

### 2.1 Checkpoint Time Difficulty setting (2026-11-14)

The General tab's Gameplay option **Checkpoint Time Difficulty** (`checkpoint_difficulty`, src/checkpoint_time.cpp)
picks the DIFFICULTY f uses, apart from the one the computer cars' pace uses (rubber_band_800A1A98,
docs/npc_cars.md#drone-catch-up). Values: Same as Race (key `Linked`, the default, Rush 2's behavior) or `0`-`5`,
labeled with their f (x1.375 ... x1.000). Hooks right after the three loads of `0x8010C211` that compute f put the
setting's difficulty in the loaded register:

| load | function | register | hook (before) |
|---|---|---|---|
| 0x800AEAC8 | race_start_timer_800AE670 (start time, checkpoints on) | $t6 | 0x800AEACC |
| 0x8008F640 | checkpoint_passed_8008F220 (checkpoint extension) | $t7 | 0x8008F644 |
| 0x800AB1B8 | race_compute_total_time_no_checkpoints_800AB1B4 (checkpoints off) | $t6 | 0x800AB1BC |

C++ that scales time by the same f reads `rush2::checkpoint_time::difficulty()` too: Rush 1's checkpoint times
(`rush2::track1::race_time`, both its own difficulty terms and the division by f) and the PS1 Time Sprint clocks
(src/time_sprint.cpp). The other readers of `0x8010C211` (the rubber band, the battle AI's skill, 0x80099EC0, the
slider) keep the race's value. It is one of the online race settings every machine must share (src/online_tab.cpp).
Checked with test_race on Las Vegas (header start 32, checkpoint 1 extension 17), race difficulty 2: Same as Race
39.2 s at the start, 60.025 after checkpoint 1; `0` 44.0 and 67.375; `5` 32.0 at the start.

## 3. Rush 2049 [V]

- **Race options** (text bank, file 0, offset 0x1FCB): TRACK, GHOSTS, LAPS, …, BACKWARD, MIRROR, FOG, WIND, GRAVITY,
  DRONES, DIFFICULTY, HANDICAP, DEATHS. There is **no CHECKPOINTS option**.
  - Rush 2's list has one: …DIFFICULTY, HANDICAP, **CHECKPOINTS**, DEATHS (main 0x800CC820).
  - 2049's "TIME REMAINING" HUD entry serves the timed battle and stunt modes.
- **Path loader** `func_800BADE0` (2049): the same code as Rush 2's.
  - It writes 90 (0x800BAE18) and 45 (0x800BAE34) into its header copy at `0x80151CE8`.
  - No code reads header+0 or checkpoint +0x1E/+0x20 afterwards. The only `0x80151CE8` users are the path, AI and
    lap functions.
- **Checkpoint handler** `func_800D24C8` (2049), the twin of `func_8008F220`:
  - It does next/loop/arming, lap counting, the lap-time stamp (`0x8002EB90` → `0x80110668`), and the lap and
    final-lap sounds and messages.
  - It **adds no time**.
- **Gate test** `func_800F8EC8` (2049): the same sign-change and x/z radius² test as Rush 2's `func_800A1468`.
- So in Rush 2049 a race is untimed. There are no per-checkpoint, per-direction or difficulty-scaled times to port.

## 4. What a 2049 race gets in Rush 2 now (before = after)

The 2049 race runs in host slot 2 (HAWAII), using 2049 path files 158+k / 177+k unchanged
(`src/rush2049/track2049_convert.cpp`). Rush 2's loader overwrites them, so every checkpoint of every 2049 track, in both
directions, gets this at difficulty d:

| | forward (files 158–163) | backward (files 177–182) |
|---|---|---|
| start | 90 × f(d) | 90 × f(d) |
| each checkpoint, lap 1 and later | 45 × f(d) | 45 × f(d) |

This is exactly what HAWAII and every other Rush 2 track gets. **These values were not changed.**

## 5. Data: file fields, driving time and slack

- Column meanings:
  - **lane-0 time per segment**: the time to drive from each checkpoint's crossing to the next one's along AI
    lane 0, at the lane's target speeds (u8 mph × 88/60 ft/s).
  - **cps/lap**: checkpoints passed per lap. Tracks with a loop-start checkpoint (NYONE, 2049 race 6) skip cp 0
    after the start, so their last segment runs from the finish checkpoint to the loop start.
  - **budget/lap**: 45 × cps/lap at f = 1.
- Times are in seconds and distances in feet. "file t1/t2" = path-file checkpoint +0x1E / +0x20, "file base" =
  header +0. Both are ignored by both games.

| path | cps | cps/lap | spine ft | file base | file t1/t2 per cp | lane-0 time per segment | lane lap | budget/lap | worst seg | worst/45 |
|---|---|---|---|---|---|---|---|---|---|---|
| VEGAS f | 6 | 6 | 24089 | 28 | 18/21, 17/17, 16/16, 23/23, 18/18, 19/19 | 19, 17, 16, 24, 19, 20 | 116 | 270 | 24 | 0.53 |
| NYONE f | 6 | 5 | 28360 | 31 | 21/45, 25/25, 15/15, 16/16, 22/22, 45/25 | 21, 26, 16, 16, 23, 64 | 145 | 225 | 64 | 1.41 |
| HAWAII f | 4 | 4 | 29684 | 36 | 26/30, 37/37, 32/32, 42/42 | 27, 38, 33, 43 | 141 | 180 | 43 | 0.95 |
| NYTWO f | 7 | 7 | 31533 | 35 | 25/30, 23/23, 22/22, 22/22, 24/24, 23/23, 22/22 | 25, 24, 22, 23, 24, 24, 23 | 165 | 315 | 25 | 0.56 |
| ALCATRAZ f | 7 | 7 | 16530 | 15 | 5/5, 8/10, 11/11, 10/10, 16/16, 13/13, 8/8 | 6, 9, 11, 11, 18, 13, 8 | 76 | 315 | 18 | 0.39 |
| LA f | 7 | 7 | 31706 | 30 | 20/25, 26/26, 17/17, 33/33, 16/16, 25/25, 18/18 | 20, 26, 18, 34, 16, 26, 18 | 159 | 315 | 34 | 0.75 |
| SEATTLE f | 4 | 4 | 20923 | 34 | 24/27, 34/34, 32/32, 21/21 | 24, 36, 32, 20 | 112 | 180 | 36 | 0.81 |
| VEGAS b | 6 | 6 | 24093 | 28 | 18/21, 18/18, 23/23, 16/16, 16/16, 18/18 | 19, 18, 23, 16, 17, 18 | 112 | 270 | 23 | 0.52 |
| NYONE b | 6 | 5 | 28171 | 28 | 18/45, 16/16, 24/24, 17/17, 16/16, 45/34 | 24, 16, 24, 18, 16, 73 | 147 | 225 | 73 | 1.62 |
| HAWAII b | 4 | 4 | 29676 | 52 | 42/46, 33/33, 37/37, 27/27 | 43, 34, 38, 27 | 142 | 180 | 43 | 0.95 |
| NYTWO b | 7 | 7 | 31533 | 33 | 23/28, 24/24, 22/22, 22/22, 23/23, 23/23, 24/24 | 23, 24, 23, 23, 24, 24, 25 | 165 | 315 | 25 | 0.55 |
| ALCATRAZ b | 7 | 7 | 16768 | 20 | 8/10, 14/14, 19/19, 11/11, 10/10, 7/7, 5/5 | 9, 15, 20, 11, 11, 7, 5 | 78 | 315 | 20 | 0.44 |
| LA b | 7 | 7 | 31706 | 27 | 17/22, 26/26, 15/15, 35/35, 18/18, 26/26, 19/19 | 18, 27, 15, 36, 18, 27, 19 | 160 | 315 | 36 | 0.80 |
| SEATTLE b | 4 | 4 | 20923 | 27 | 17/20, 32/32, 36/36, 21/21 | 18, 33, 37, 23 | 111 | 180 | 37 | 0.82 |
| 2049 race 1 f (158) | 6 | 6 | 12896 | 19 | 8/9, 8/9, 10/10, 9/9, 12/12, 7/7 | 4, 4, 10, 9, 12, 8 | 48 | 270 | 12 | 0.28 |
| 2049 race 2 f (159) | 6 | 6 | 18246 | 24 | 12/14, 15/15, 13/13, 14/14, 6/6, 19/19 | 13, 15, 13, 15, 7, 20 | 82 | 270 | 20 | 0.44 |
| 2049 race 3 f (160) | 5 | 5 | 17914 | 25 | 13/15, 6/6, 12/12, 15/15, 17/17 | 13, 6, 12, 16, 18 | 65 | 225 | 18 | 0.40 |
| 2049 race 4 f (161) | 8 | 8 | 19045 | 26 | 13/16, 23/23, 23/23, 12/12, 6/6, 17/17, 20/20, 4/4 | 14, 15, 8, 12, 7, 18, 21, 5 | 99 | 360 | 21 | 0.46 |
| 2049 race 5 f (162) | 6 | 6 | 17458 | 21 | 9/11, 25/25, 13/13, 15/15, 15/15, 15/15 | 15, 16, 18, 11, 9, 10 | 79 | 270 | 18 | 0.41 |
| 2049 race 6 f (163) | 6 | 5 | 32535 | 33 | 20/45, 18/18, 35/35, 34/34, 23/23, 45/7 | 20, 19, 35, 35, 23, 48 | 161 | 225 | 48 | 1.07 |
| 2049 race 1 b (177) | 5 | 5 | 12976 | 18 | 7/8, 12/12, 9/9, 10/10, 9/9 | 8, 12, 9, 10, 10 | 50 | 225 | 12 | 0.28 |
| 2049 race 2 b (178) | 6 | 6 | 18234 | 31 | 19/21, 6/6, 13/13, 12/12, 17/17, 10/10 | 19, 6, 14, 12, 17, 11 | 80 | 270 | 19 | 0.43 |
| 2049 race 3 b (179) | 5 | 5 | 17914 | 32 | 20/22, 19/19, 13/13, 7/7, 15/15 | 21, 19, 13, 7, 16 | 77 | 225 | 21 | 0.47 |
| 2049 race 4 b (180) | 7 | 7 | 19045 | 17 | 4/7, 24/24, 19/19, 8/8, 14/14, 23/23, 9/9 | 5, 24, 19, 8, 14, 24, 9 | 103 | 315 | 24 | 0.53 |
| 2049 race 5 b (181) | 6 | 6 | 17458 | 26 | 14/16, 10/10, 12/12, 20/20, 14/14, 15/15 | 14, 10, 12, 21, 14, 15 | 87 | 270 | 21 | 0.46 |
| 2049 race 6 b (182) | 6 | 5 | 31490 | 31 | 18/45, 17/17, 33/33, 36/36, 25/25, 45/8 | 18, 18, 34, 36, 26, 42 | 155 | 225 | 42 | 0.93 |

Observations:

- **file t1 ≈ lane-0 segment time** on every ordinary checkpoint of both games.
  - The editor wrote expected driving times, plus a few s on lap 1 (t1 > t2 at cp 0).
  - The 45s and 7/8 at the loop checkpoints are stale editor values.
- The 2049 lane speeds are tuned for 2049 cars, which are faster than Rush 2's (max lane speed 160–190 mph, against
  160–194 in Rush 2). A Rush 2 car may take somewhat longer than these times, but the margins are large.
- Only 2049 race 6 (both directions) has a segment near 45 s: the finish → loop-start segment, cp 5 → cp 1. That is
  the same shape as NYONE, whose equivalent segment is 64/73 s. Race 6 still banks 225 − 161 ≈ 64 s per lap
  (forward) at f = 1.

## 6. Checkpoint registration on 2049 paths [V]

- The gate only counts the car's *next* checkpoint. It counts when the car's side of the gate plane changes (in
  either direction) while the car is within the gate radius in x/z.
  - A crossing outside the radius is never retried, so it would stop all later extensions.
- I replayed this test along all 4 AI lanes of every 2049 race path (3 laps, both directions). Every lane registers
  every checkpoint of every lap. The counts match the Rush 2 tracks' (ncp × 3 minus the start-only gates).
- On the worst lane, the gate is passed at most 0.65 × radius from its centre (2049 race 6 cp 5, radius 420 ft).
  Everything else is under 0.4 × radius. Rush 2's tracks give the same picture.
- Branches (shortcuts) leave and rejoin between gates. The one branch that rejoins past a gate (race 5 forward,
  branch 4 around cp 1) curls back through that gate at about 0.4 × its radius, so the gate still counts.

## 7. Conclusion and options

- **Cause:** none was found in the timing data or code. A 2049 race already gets Rush 2's normal timing: 90 × f
  start and 45 × f per checkpoint, in both directions. By the same driving-time measure, that is as generous or
  more generous than on the Rush 2 tracks.
- Rush 2049 itself has no race timer, and its files' per-checkpoint fields are driving-time estimates that would
  shorten the time. So there is no "Rush 2049 checkpoint time" to restore, and no host-slot table to swap:
  - the 90/45 constants live in `func_80093300`'s code
  - the HAWAII slot has no timing entry
- **What to check in game** (2049 track, checkpoints ON):
  - Does the remaining time jump up by about 45 × f at each checkpoint gate? If it doesn't, the extension isn't
    registering. Note where, and capture the car's position: the gate test of §6 or the human's route would then be
    the issue, not the values.
  - Is the difficulty (`0x8010C211`) in 0–5 during the race? A value above 5 makes f < 1.
- **Optional (behaviour change, not applied): make 2049 races untimed like Rush 2049.** Raise the time allowed when
  the race starts, using a hook at the instruction after the start-time store in `func_800AE670`. Rush 2's own
  practice mode uses 999 s.

  ```c
  // func_800AE670 at 0x800AEB20 (after `swc1 $f0, 0x8010C204` at 0x800AEB1C, checkpoints ON) and
  // at 0x800AEB40 (after the store at 0x800AEB3C, checkpoints OFF): 0x8010C204 = time allowed.
  extern "C" void rush2_track49_race_time(uint8_t* rdram, recomp_context* ctx) {
      if (rush2::track2049::race_track() != 0 && MEM_B(0, (int32_t)0x8010C3F0) == host_slot) {
          float t = 999.0f;   // never runs out within a race; checkpoint extensions still add on top
          MEM_W(0, (int32_t)0x8010C204) = *(uint32_t*)&t;
      }
  }
  ```

  `us.toml`: `[[patches.hook]] func = "func_800AE670"`, `before_vram = 0x800AEB20` and a second one at
  `0x800AEB40`, `text = "    rush2_track49_race_time(rdram, ctx);"`. `rush2_hooks.h`:
  `extern "C" void rush2_track49_race_time(uint8_t* rdram, recomp_context* ctx);`

## 8. Car tracker, checkpoint window and respawn point (Rush 2 vs 2049 vs SF Rush) [V]

Decompiled with `tools/rush2049/decomp.py` (r2 80090570 80090A40 800A1468 8008F220 8008F8B0; 49 800D348C 800D3B28
800F8EC8).

- **Gate test** `func_800A1468`: only the car's *next* checkpoint (car +0x800) is tested. The side of its plane is
  kept in car +0x802; a side change counts only if the car is then inside the gate radius in x/z. A plane crossing
  outside the radius is consumed and never retried. Skipped while the car is wrecked (car +0x6C8 != -1).
- **Tracker** `func_80090570` (human cars; 2049 `func_800D348C`): nearest spine point in 3D, and nearest branch point
  if the spine is over 40 ft away (1600 = 40²). In a race the result is then **confined to the checkpoint window**:
  spine indices [cross(last cp, car +0x7FE), cross(next cp, car +0x800)), or up to the spine's end when next < last.
  A nearest point outside the window is replaced by the nearest point *inside* it. Branches: kept if the last cp
  has a crossing on the branch, or the branch's cp (+8) is the last cp; else the spine window is used.
- **Consequence:** a missed gate freezes both the checkpoint count and the window, so every later respawn lands in
  the stretch before the missed gate, however far the car has driven since.
- **Respawn** `func_80090A40` (2049 `func_800D3B28`): from the tracker's point it moves on along the route
  (`func_8008F8B0`) by `min(lane speed mph x 1.4667, 146.67) x k x (seconds wrecked + T) / 20` points (20 ft each),
  k = 0.8 (1.1 if state +0x300 was set). If that passes the next cp's crossing, `func_8008F220` runs (cp counted).
  - T = 1.5 s in Rush 2 (literal at the `temp_f2_2` add), wreck delay 3.5 s (`func_8008E40C`). T = 0.01 s in 2049
    races (0x80124194; game types 0, 1, 2, 3, 5), wreck delay 5 s. Both come to 5 s, about 29-40 points =
    590-800 ft past the nearest point: **no difference between the games here.**
  - 2049 only: a branch with type byte 2 (files 159 br 7, 178 br 5) is a respawn-in-place zone: the tracker skips
    the window and the car is put on the branch point itself, +3 ft, speed 0. Rush 2 has types 0 and 1 only.
    Ported: `rush2_track49_respawn_zone*` in `src/rush2049/track2049.cpp` (hooks at 0x80090758, 0x80091368, 0x800918E8).
  - 2049 battle (game type 6, `0x8014A110`): the tracker `func_800D348C` doesn't look for the nearest point. From a
    random spine point on (its LCG `0x8011735C`), it takes the spine point farthest in x/z from every other car, each
    car taken at its respawn point (+0x660) while it is being put back (+0x6C4 >= 0), else where it is (+0x22C); it
    also calls the battle overlay's `func_8038CA24(car)`. `func_800D3B28` then moves on 0 points (+0x6BC = 0 in types
    1, 4, 5, 6) and leaves the car standing (no 0x3A speed). Ported for the battle arenas, players and computer
    opponents alike: `rush2_track49_respawn_zone_place` (hook at 0x800918E8 in `respawn_place_car_80090A40`) puts the
    car on that point +3 ft with no speed, and gives `func_8006D348` (which faces it along the route) that point
    (0x1A8($sp), loaded into $a1 by the hooked instruction), branch -1 (0x1B0($sp), $a2) and $a3 = 1. Rush 2 offsets
    are 2049's + 4 here (+0x664 respawn point, +0x6C8 the respawning index), and the position is +0x224.
  - The helpers are the same code in both games (`func_8008F8B0` = 2049 `func_800D2928`, `func_80090514` =
    `func_800D3430`); no other difference was found.
- **SF Rush** has the same gate test (`func_800A0BB8`) with its own radii, 316-1000 ft (rush1_research section 6).
  The converted path files had 60-507 ft gates (most 300; track 6 f cp 3/4 = 60/80, b cp 0/2/5 = 60/80/60; track 7
  cp 4 = 100/130), so a route passing a gate further out than that missed it. Fixed by giving the gates Rush 1's
  radii after load (`rush2::track1::race_time`).
