# Hitch profiler

A sampling profiler built into the game for finding what causes long frames. It is off unless `RUSH2_HITCH_LOG` is set.

## Recording

`RUSH2_HITCH_LOG=<file>` starts a sampler thread in `rush2::hitch::start` (`src/hitch_profiler.cpp`, called first
thing in `main`). About once a millisecond it suspends each thread that used CPU since the last tick, walks its stack
and resumes it. It always samples the game threads (`[Game] N`), `Gfx Thread`, `VI Thread`, `SP Task Thread`,
`RT64 Present` and `RT64 Workload`, so a thread that is blocked shows what it is waiting on. The log also gets frame
markers:

- `D` / `d`: ultramodern's gfx thread hands a display list to RT64 / RT64 returns (`send_dl`).
  `ultramodern::displaylist_profile_mark`, in `lib/N64ModernRuntime` `events.cpp`.
- `P` / `p`: RT64's swap chain present begins / ends. `RT64::presentProfileMark`, in `lib/rt64`
  `rt64_present_queue.cpp`.
- `rush2::hitch::mark(tag)`: any named marker.

The stack walk reads each module's unwind table (`.pdata`) itself and never calls `RtlLookupFunctionEntry`, the heap or
the loader while a thread is suspended, so it can't deadlock on a lock the suspended thread holds. A 50-second run
logs about 30 MB. The game behaves the same with it on: a race run with and without it reached the same race clock.

A run from launch to a race, stepping through the track and car selects (shots.py gives the timing; set the car and
track explicitly):

```
RUSH2_HITCH_LOG=$PWD/tmp/hitch/prof.log RUSH2_TEST_CAR=7 RUSH2_TEST_TRACK=0 python tools/rush1/shots.py tmp/hitch \
  "8:START,11:A,13.5:A,16:DR,...,26:A,28:DR,...,35.5:A,38:A:14" 52
```

`A` at 26 picks the track, `A` at 35.5 picks the car, and the race starts about 0.1 s later. `38:A:14` holds the gas
for 14 seconds.

## Reading

`python tools/hitch_profile.py LOG [--ms 20] [--game-ms 45] [--from S] [--to S] [--folded DIR]`

A hitch is a gap of more than `--ms` between two presents (what RTSS's frame time graph shows) or of more than
`--game-ms` between two display lists. The game runs at 30 fps, so a 33 ms display list gap is normal. For each hitch
the report lists the threads that used the most CPU in it, plus the game, gfx, present and workload threads. For each
thread it shows the dominant call path: the chain of functions that at least half of its samples went through, then
where that chain branches. Symbols come from the PDBs, and recompiled functions keep their names
(`race_setup_800A5ADC`). `--folded` writes folded stacks per hitch for a flame graph viewer such as speedscope.

A thread's CPU comes from `QueryThreadCycleTime`, so a thread that only wakes briefly reads close to 0 ms. A
`send_dl` that blocks shows up as a long `D` to `d` span with the gfx thread waiting inside RT64.

## Findings, 2026-10-14

Measured on Las Vegas, car 7, with RTSS running. "Per build" means the work is cached on disk keyed on the
executable, so it happens on the first launch of every new build (and on every user's first launch of a release).

| When | Cost | Thread | Code | Per |
|---|---|---|---|---|
| Boot, before the first frame | 1.1 s | game | `rush2_cheats_frame` -> `rush2::car2049::prepare_bars` -> `find_ranges` (test car runs for Accurate Car Stats) | build |
| First frame | 50 ms | gfx | `RT64Context::check_texture_pack_actions` -> `TextureCache::loadReplacementDirectories` (JSON parse, `waitForAllStreamThreads`) inside `send_dl` | launch |
| First frames | a few ms | present | RmlUi building font glyphs (`Rml::BuildGlyphMap`) in `draw_hook` | launch |
| Opening track select | 50 ms | game | `trackselect_init_draw_803AB294` -> `rush2_track49_select_init` -> `refresh_mods` -> `track_mod::menu_art` -> `convert_and_store` -> `track_mod::convert` | build |
| Race start, first frame | 45-51 ms `send_dl` | gfx | `State::fullSync` -> `TextureCache::waitForGPUUploads` (about half; the `RT64 Texture` thread creates D3D12 textures and waits for the copy); `rush2::upscale::Upscaler::textureDrawn` -> `decode_tmem` (about 8 ms); TMEM hashing | launch |
| Race start, next 3 frames | 16-20 ms `send_dl` each | gfx | same | launch |
| Race start | about 3 s CPU over 1 s | 16 `RT64 Shader` threads | `RasterShaderCache` compiling specialized shaders with DXC (idle priority, not persisted). They fight the gfx thread for the process heap lock (`RtlEnterCriticalSection` in `RtlAllocateHeap`) | launch |
| After any hitch | 4-5 frames at 27-34 ms | present | the variable refresh rate pacing in `WorkloadQueue::renderThreadLoop` presents one frame per game frame until it catches up (present thread waits in `threadLoop`) | follows the others |
| Anywhere | about 20 ms | present | RTSS's own hook (`RTSSHooks64!VideoCaptureEx` in `D3D12SwapChain::present`) | RTSS |

Browsing every track and car and driving 10 seconds of the race had no hitches once the build's caches existed.

## Fixed

Checked with the same run on the first launch of a new build (this build's cache entries deleted), 2026-10-14:

- Car bar ranges: `prepare_bars` works through the setups `plan_ranges` lists about 6 ms a menu frame (`run_ranges`);
  a car select reached before they're done finishes them (`find_ranges`). The first frame now comes at 1.0 s instead of
  2.4 s, and the stored ranges are byte for byte the same.
- Custom tracks: `rush2::track_mod::prepare_in_background` (polled from `rush2_cheats_frame`) converts the enabled mods'
  tracks into the disk cache on a background thread when the enabled mods change, so the track select only reads
  the cache. Opening the track select no longer hitches.
- Upscaler: `textureDrawn` copies TMEM into a job and the upscaler's decode thread (`decode_loop`) decodes it, so
  `decode_tmem` is off the gfx thread.

2026-10-19: every conversion now runs at launch in the background (docs/prewarm.md): the boot's car conversions, the
first race of each added track, the track select's mod track reads and the car select's save write are gone from the
game thread.

`--brief` prints one line per busy game or graphics thread per hitch, and `--sym-path DIR` reads a copy of the run's
exe and PDB (another session's build replaces build/'s, after which a log can't be symbolized).

Still open: the race start's first frame (about 50 ms, almost all `TextureCache::waitForGPUUploads`), texture packs
loading in the first `send_dl`, the specialized shader compiles on every launch, and the pacing after a hitch.
