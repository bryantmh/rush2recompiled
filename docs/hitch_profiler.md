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

The walk reads only inside the thread's stack (the bounds in its TEB), so it never raises an exception (dispatching one
takes locks the suspended thread may hold), and modules unloaded since are dropped from its list. A run of 2026-10-21
stopped logging at 57 s with a background thread left suspended; the scanner thread now resumes any thread the sampler
has held for more than 100 ms and the log gets a `watchdog` mark for it.

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

`--sends MS` lists every `send_dl` of at least MS ms between `--from` and `--to` (how long RT64 held the gfx thread)
and their median, p90 and max, for comparing runs. `--timeline S` prints presents and game frames per S-second bucket,
`--blind MS` the times the sampler itself stalled (a thread it suspended was inside a long kernel call; its samples are
missing there, so a report of that stretch is incomplete), `--busy` each thread's running samples only.

## Race start textures

The race start's first frames bring hundreds of new textures, and RT64 uploads each one (copies TMEM, decodes it on the
GPU) before the frame that draws it can go on. Measured 2026-10-21 (`tools/test_race.py`, clean data folder):

| Case | Race start `send_dl` |
|---|---|
| Las Vegas (track 0), no texture list | 34-38 ms one frame, 10-14 ms the next |
| Arcade Alcatraz, second run, no texture list | 30-46 ms for 5 frames |
| Arcade Alcatraz, second run, with its texture list | at most 11 ms (no hitch) |

Texture prewarming (`src/texture_prewarm.cpp`) is what fixes it: the textures a track's race start uploaded are kept
per track (`<config>/shader_cache/race_textures_<track>.bin`, recorded through `RT64::TextureUploadObserver`) and handed
to RT64 again at that track's car select (`RT64::prewarmTexture`, uploaded in batches of 24 behind the game's own),
so the race finds them in the texture cache. A recorded texture is kept only if it's still in RT64's cache 12 frames
after its upload (`sift`), which drops one-frame render-to-texture copies RT64 evicts at once. The file header carries
the `LoadTile` size and RT64's TMEM hash version, so a list from another RT64 version is ignored. Picking the same track
again without racing doesn't prewarm it a second time.

Tried and taken out (2026-10-21): making RT64's frame never wait for an upload on the GPU. Each batch had its own
command lists and fence (so a batch never waited for the one before), was published as soon as it was submitted and
signaled a semaphore the framebuffer worker's next submission waited on; TMEM textures were made ahead and evicted
decoded textures reused by size. It changed nothing measurable: without a texture list the gfx thread waited about as
long, on the framebuffer submission's fence instead, because the GPU itself is busy decoding the textures with the
frame. Racing frames were 6-9 ms `send_dl` with and without it. The upload wait on the CPU isn't the cost to cut.

The specialized shaders RT64 compiles are kept on disk (`rt64_shaders_<format>.bin` in the same folder,
`RasterShaderCache` in the RT64 patch) and all built at launch, so a race no longer compiles them on first use.

Only one running game writes that file. It holds an OS lock on `<file>.lock` for as long as it runs, and a second
copy of the game started alongside (two-instance online tests, tools/online_test.py) only reads it. Without the lock
both games appended at once: their buffered writes interleaved, and because two games in the same lockstep race compile
the same shaders in the same order, the mixed file still parsed. One record kept its sizes but carried a pixel shader
whose bytes restarted 4096 bytes in, so D3D12 failed to make its pipeline, and every later launch crashed in
`SetPipelineState(null)` when that shader drew (online Stunt arena, 2026-10-30). The loader now checks each DXIL record's
container and drops a damaged record and everything after it. `python tools/shader_cache_check.py <file>` reports damaged
and duplicate records, and `--extract N` writes record N's shaders out for `dxc -dumpbin`.

Still open: the pacing after a hitch, and a track's first race (no texture list yet).
