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

**Fix** (`rush2_track49_race_time` in `src/track2049.cpp`, hook in `func_80093048` at 0x80093298): on a 2049 track
the computed start time and extensions are multiplied by the path's lane 0 speed ÷ 139.3 when that is above 1, which
is what they would be at Rush 2's lane speeds. Track 1 forward then starts with 19 (23 s at difficulty 2).

## 1. Summary

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

## 2. Rush 2 timing [V]

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
- The recompiled code (`RecompiledFuncs/funcs_16.c`, `func_80093300`) has the same 0x5A / 0x2D constants. Nothing
  in `us.toml` or `src/` hooks the timer.

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
(`src/track2049_convert.cpp`). Rush 2's loader overwrites them, so every checkpoint of every 2049 track, in both
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
