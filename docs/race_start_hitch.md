# Race start hitch

The first frames of every race come 33, 17, then 66 to 135 ms apart instead of every 33 ms, so the start of the
race's flyover visibly hitches. Measured 2026-10-09 (Las Vegas and SF Rush track 10, one player, Variable refresh
rate). Two causes; the first applies to every track.

## How the game paces frames

The scheduler loop in `game_thread_main_init_800B4EB0` starts a game frame only when the VI count and the graphics
tasks in flight allow it, and measures each frame's length in VIs: `frame_dt` (0x80023028) = `vi_count` (0x8002331C)
minus `vi_count_last_frame` (0x80023024), times the VI period (0x800B53A0-0x800B5460). `game_main_loop_frame_800B0228`
adds `frame_dt` to `timer_game_time` (0x80117488) once per frame. The race start flyover camera moves a fixed step
per frame, not per second, so a long frame shows as a stall and a short one as a lurch.

The game can't start a frame until RT64 has finished the previous graphics task: ultramodern's gfx thread calls
`send_dl` synchronously and only then sends DP complete (`gfx_thread_func`, ultramodern `events.cpp`).

## 1. RT64 renders the frame on the game's graphics thread

RT64's `renderToRAM` defaults to on (`rt64_emulator_configuration.cpp`) and neither the game nor the recomp turns it
off (extended GBI `G_EX_SETRENDERTORAM`). With it on, `State::fullSync` (`src/hle/rt64_state.cpp`) renders the
workload on the GPU and waits for it inside `send_dl`, on the game's critical path:

| | `send_dl` | of which |
|---|---|---|
| menus | ~1.5 ms | |
| race, steady | ~7 ms | |
| first race frame | 46-86 ms | ~33 ms draw data upload (`uploadDrawData`, output buffers, transforms), ~79 ms the render-to-RDRAM block (new textures, render targets) |
| next 3 frames | 12-28 ms | |

The workload queue never made `send_dl` wait (`advanceToNextWorkload`'s barrier: 0 ms). Not changed: turning
render-to-RDRAM off takes RT64 off the game's path, but it has to be checked first whether Rush 2 reads its
framebuffer on the CPU anywhere (pause screen, menus), and it changes base behavior, so it would need a toggle.

## 2. Converting the added track inside the race setup

The race setup runs on the race's first frame: `race_start_timer_800AE670` -> `race_setup_800A5ADC` ->
`race_load_track_800A5110` -> `track_assets_queue_800A4C98`, whose first instruction is hooked by
`rush2_track49_load` (`src/rush2049/track2049.cpp`). For an SF Rush track that calls `rush2::track1::load`
(`src/rush1/track1.cpp`), which converted the track from the Rush 1 ROM there: 26 ms (applying it takes 0.4 ms). Rush
2049 tracks convert at the same point. A stock Rush 2 track spends ~16 ms in `race_setup` on that frame.

The conversion used to be kept only for the last track raced in the session. It is now cached on disk.

## Converted track cache

`src/track_cache.cpp` (`include/track_cache.h`) keeps each converted track in `<app folder>/track_cache/`
(`rush1_<t>.bin`, `rush2049_<k>.bin`), so a track is converted once rather than on its first race of every
session:

- SF Rush: `convert` in `src/rush1/track1.cpp` stores the converted track and its in-race logo. `record_seed` reads the
  lap times from the cache too, instead of converting a track to get them.
- Rush 2049: `convert` in `src/rush2049/track2049.cpp` stores the converted track. The mover, texture animation, prop and
  battle data are still built from it on every load, and the shared model scan of the Rush 2 ROM only runs when a
  track has to be converted.
- An entry is used only if its key matches: track, slot prefix, source hash (the whole Rush 1 ROM; the Rush 2049 N64
  ROM, or for a Dreamcast disc the track's geometry and collision files) and the executable's size and modification
  time. Any new build converts again, so a cached track never outlives a change to the converters. A file that
  doesn't read back completely is ignored and replaced.
- The serializer writes the struct fields in a fixed order. `static_assert`s on the sizes of the converted track
  structs (and `Timing`, `TexFlipbook`, `TexScroll`) fail the build when a field is added without being serialized.

Checked: one launch writes the entry and the next reads it without rewriting it, for SF Rush tracks 4 and 6 and
2049 track 3, and a race from a cached track looks the same as one from a fresh conversion.

## Measuring

`tools/time_calls.py` instruments every call in chosen recompiled functions and logs the slow ones
(`RUSH2_TIME_CALLS=<log>`); `apply --copy <dir>` writes the instrumented files elsewhere for a private build, as
other sessions regenerate `RecompiledFuncs`. The RT64 and ultramodern timings above came from temporary timers in
`State::fullSync` and around `send_dl`.
