# Launch builds

Everything the game converts from the other games' ROMs and from mods is built before the game starts, behind a
progress screen on the launcher (`src/prewarm.cpp`, `include/prewarm.h`), so the boot only installs and no menu, car
select or race start converts anything. The results go into the disk cache (`src/track_cache.cpp`,
docs/race_start_hitch.md#converted-track-cache), keyed on the build, so after a new build or install the first launch
builds them once.

## What is built

`launcher_update` (src/main.cpp) calls `rush2::prewarm::start` once the Rush 2 ROM is valid and starts the game only
when `rush2::prewarm::ready()`. The pass (`build_pass`) loads the ROM itself (`recomp::load_stored_rom`; the game
loads it again when it starts) and runs, each on a thread of its own at normal priority:

- the arcade mods rebuilt when out of date (`rush2::arcade2049::refresh_now`, docs/arcade2049.md), then from what that
  installs the car mods' plan (`rush2::car_mods::prepare`, below) and the enabled mods' custom tracks
  (`rush2::track_mod::prepare`, which also keeps their menu art);
- the Rush 2049 cars and part models of the source in use (`rush2::car2049::prepare`);
- California Speed's cars and name logos (`rush2::calspeed::prepare_cars`);
- the Accurate Car Stats bars' ranges (`rush2::car2049::prepare_bar_ranges`, below);
- SF Rush's tracks (`rush2::track1::prepare`, which also keeps their lap times for the records screen's seeds);
- the SF Rush car stripes' decals (`rush2::car1stripes::prepare`, into `stripe_cache`, docs/rush1_research.md section
  11), with the Rush 1 ROM in;
- Rush 2049's races, obstacle course, stunt and battle arenas and California Speed's tracks as its Extended Scenery and
  Billboard Trees options are set (`rush2::track2049::prepare`);

then the track select's art container (`prepare_menu_container`). A game whose tracks option is off is skipped. An
entry already on disk for this build is skipped by its key alone (`has_2049`, `has_rush1`, `has_blob` read only the
key); a Dreamcast disc's tracks are keyed on their own files, which are read for it.

Measured 2026-10-21 (`tools/test_race.py menus --show`, every game's ROM, five mods): with nothing built and the arcade
mods out of date the pass took 6.3 s (the arcade rebuild and reinstall about 2.3 s, the custom tracks converted from
it, Rush 2049's and California Speed's tracks 3.3 s at once with them); with everything built 0.4 s. A ROM that
doesn't load skips the pass and builds after the boot as before.

After the pass, about once a second in the background: another Rush 2049 source or California Speed ROM picked in
the Games tab (their cars, which `check_source` / `check_cars` install in the menus), a tracks option turned on, other
scenery settings, other enabled mods. Each change builds what it needs; the settings have no apply button, so nothing
waits for it. These builds wait for `rom_image_final` first: the boot grows the ROM image once, freeing the buffer
the pass read.

At the car select (`rush2::prewarm::frame`, from `rush2_cheats_frame`), the chosen track's cache entries are read into
memory (`track1::preload`, `track2049::preload`, `calspeed::preload_track`, `track_mod::preload`), so the race start
reads nothing from the disk.

### The progress screen

The launcher has two faces only: Locate ROM (no valid ROM) and this screen; it never offers Start Game (its button
keeps the Locate ROM title, and a ROM just located goes straight on to the pass). With a ROM the launcher menu is
invisible while the pass runs, and `show_build_progress` (src/main.cpp) shows the logo and the bar, with a line under it
("BUILDING CUSTOM TRACKS"), only once something is being built (`Progress::building`: a track cache lookup missed,
`track_cache::misses()`, or an arcade mod is rebuilt, `a49::rebuilt()`), never on a timer: a launch with everything
built shows nothing on any computer (0 misses there, measured). The bar and the buttons stay in the layout and only
their visibility changes: the menu sits from the bottom, so hiding the buttons outright moved the logo. The fill is positioned absolutely from the track's
left edge, without overflow clipping on the rounded track (with it only the fill's end was drawn).

`rush2::prewarm::progress()` gives the pass's fraction by time: each job has about how long it takes when it has work
(`Job::seconds`, replaced by this computer's own time from the last launch it had work, `<config>/track_cache/
launch_times.txt`), a running job counts for `seconds * (1 - exp(-ran / seconds))` of it (always moving, slower once it
runs long) and a finished job for the time it took (a job that had everything built drops out). The bar eases toward
that every frame and never goes back; once the pass is over it runs to the end in about a third of a second before the
game starts. `RUSH2_TEST_BUILD_SCREEN=S` keeps the screen up S seconds after the pass to look at it.

### Accurate Car Stats ranges

The bars' ranges drive the game's own car physics (car_init_setup_8008DBA0, car_drivetrain_800712EC,
car_tire_model_8006A2FC, car_steering_yaw_terms_80069E74 and the port's hooks in them; no OS, sound or indirect calls)
through every selectable car's setups. `prepare_bar_ranges` runs them in the pass on a 12 MB memory image of its own,
made as the boot makes the game's: Rush 2's code and data (`rush2::assets::boot_image`: ROM 0x1000 at 0x80000400, the
main segment inflated at 0x800539E0, which holds every table and constant the test reads), then the parts of
`rush2_heap_init` the test car reads (`init_tables`, `init_physics`, `rush2::calspeed::build_physics_image`; not the
cars' models or the mods, which the ranges leave out). The stack is at 0x80300000 in the image. The result goes to the
disk cache (`car_bar_ranges_*`) under the key the boot's `plan_ranges` makes from the real RDRAM (the stock rows of
every type but the mod cars, whose rows the mods set at boot), so the game only reads it. Measured 2026-10-27: the
same key and the same eight values from the image as from the game's memory; 3.0 s in a new build's pass, beside the
tracks' 6 s, so the pass took no longer.

The per-frame slices (`rush2::car2049::prepare_bars`, 10 ms on the logos, title and attract demo, 6 ms in the menus)
are left for a menu change of the inputs (Car Speeds, the 2049 cars or their source, Torque Rebalance, Accurate Car
Stats turned on). Before the pass made them, a car select reached before the slices finished (logos skipped on a new
build) ran the rest in one frame (`find_ranges`): a black screen of a second or more.

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
