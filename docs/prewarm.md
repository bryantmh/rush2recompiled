# Background builds

Everything the game converts from the other games' ROMs and from mods is built off the game thread as soon as the app
starts (`src/prewarm.cpp`, `include/prewarm.h`), so no menu, car select or race start converts anything. The results
go into the disk cache (`src/track_cache.cpp`, docs/race_start_hitch.md#converted-track-cache), keyed on the build,
so after a new build the first launch builds them once, in the background.

## What is built, in order

`rush2::prewarm::start` runs from `launcher_init` (src/main.cpp). With a valid ROM the game starts right away, so the
cars get about a second before the boot (`rush2_heap_init`) installs them:

1. The cars, each on a thread of its own at normal priority:
   - the Rush 2049 cars and part models of the source in use (`rush2::car2049::prepare`);
   - California Speed's cars and name logos (`rush2::calspeed::prepare_cars`);
   - the car mods' plan (`rush2::car_mods::prepare`, below), once `recomp::get_rom` is set.

   The cars' cache entries are read into memory as well (`track_cache::preload_blob`), so the boot only installs them.
2. Every track, at below-normal priority on two threads: Rush 2049's races, obstacle course, stunt and battle arenas
   (`rush2::track2049::prepare`), California Speed's as its Extended Scenery and Billboard Trees options are set (same
   call), SF Rush's (`rush2::track1::prepare`, which also keeps their lap times for the records screen's seeds) and the
   enabled mods' custom tracks (`rush2::track_mod::prepare`, which also keeps their menu art). A game whose tracks
   option is off is skipped. An entry already on disk for this build is skipped by its key alone (`has_2049`,
   `has_rush1`, `has_blob` read only the key); a Dreamcast disc's tracks are keyed on their own files, which are read
   for it.
3. Then once a second it looks for changes: another Rush 2049 source or California Speed ROM picked in the Games tab
   (their cars, which `check_source` / `check_cars` install in the menus), a tracks option turned on, other scenery
   settings, other enabled mods. Each change builds what it needs.

At the car select (`rush2::prewarm::frame`, from `rush2_cheats_frame`), the chosen track's cache entries are read into
memory (`track1::preload`, `track2049::preload`, `calspeed::preload_track`, `track_mod::preload`), so the race start
reads nothing from the disk.

## The game and the background on the same entry

`track_cache::claim(entry)` is a mutex per cache entry, held around each load-convert-save. When the game needs an
entry a background thread is making, it waits on the claim for the rest of that conversion and then reads the disk,
instead of converting it again. Hold one claim at a time; the game's own locks (`track_mutex`) may be held around a
claim, never the other way round. The cache's file reads and writes take no lock of their own: each writer has its own
temporary file, renamed into place.

A preloaded entry (`preload_*`) is held until its next load takes it, at most 64 MB in all (the oldest dropped). Its key
is still checked by that load; one written again since falls back to the disk.

## Car mods

`rush2::car_mods` is split into a plan and an install. `make_plan` reads and converts the enabled mods' model mods,
liveries, new cars and car replacements (no game memory: CARPALETTE is read from the ROM's own asset table,
`rush2::assets::rom_original`), with each mod's .rtz opened once for the whole plan (`ZipBatch`; a zip's central
directory is read and sorted on every open, and one open per file read took most of the 270 ms the boot spent on car
mods with five mods enabled). `install` puts a plan into the game: the patcher, the new cars' slots, the replacements,
skipping what is installed already from the same files.

- At boot `init` installs the plan `prepare` made for the same mods (keyed on the enabled mods and each mod's file
  time), waiting for it while it is being made, or makes one itself when there is none.
- In the menus `check` sees mods turned on or off (or a reinstall, `reload`), has the plan made on a background thread
  and installs it on a later frame once it is ready.

## Custom tracks

`rush2::track_mod` keeps, per track, what `read_files` found (the files' hash, cache key and track.toml) with a stamp of
the files (the .rtz's time and size, or each file's in a folder mod), and its menu art once made or read. While the
stamp is unchanged the track select (`menu_art`) and the race (`load`) read none of the mod's files: reading them took
about 100 ms when the track select opened with five mods enabled. A stock sky's model is read from the ROM
(`rom_original`), so the tracks can be built before the game boots.

## Saves

`rush2::data_files::write` updates the file's state in memory and hands the file's text to a writer thread
(`Writer`), which replaces the file as before (temporary file, the old one kept as .bak). The latest text per file
wins; what is queued at exit is written by the writer's destructor. Replacing the 1 KB save file took about 100 ms on
the game thread at the car select.

## Measured, 2026-10-19

Hidden runs through the menus (`tools/test_race.py menus`, five mods enabled, every game's ROM in), with
`tools/hitch_profile.py --brief`, game-thread frames over 45 ms:

| When | Before | After, first launch of a build | After, later launches |
|---|---|---|---|
| Boot, before the first frame | 600 ms game thread (car conversions, car mods) | 93 ms (installing the car mods), inside the boot's own 400 ms gap | same as a stock boot |
| Track select opens | 107 ms (mod tracks' files) | 12 ms (building the menu container) | none |
| Car select | 68-100 ms (save write) | none | none |
| Race start, added track | 400-700 ms first race of each track per build; 100-170 ms after (stock 67-84 ms) | only RT64's | only RT64's |

Every track was in the cache 3.6 s after launch on the first launch of a build (0.1 s on later ones, keys only).

Still on the game thread: installing changed car mods (`car_mods::check` after mods are turned on or off in the
Mods menu, or the arcade build reinstalls its mod): about 40 ms, mostly the liveries' sheen layers and damage bodies
built in the patcher (`append_liveries`, run by `set_patcher` for each car asset).

## Measuring

`tools/test_race.py menus` runs a hidden game through the menus from a data folder of its own (empty folder: every
cache cold), and `RUSH2_HITCH_LOG` with `tools/hitch_profile.py` names what each long frame was doing
(docs/hitch_profiler.md). Other sessions rebuild build/ while a run goes; copy the exe and its PDB aside right after
linking and pass that folder as `--sym-path`, or the log can't be symbolized.
