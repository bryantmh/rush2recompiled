# Settings menu

The recomp menu (Escape) is recompui's tabbed modal. Every time a tab is selected, `TabbedModal` deletes the open
page and builds the new one from scratch (`lib/RecompFrontend/recompui/src/elements/ui_modal.cpp`,
`TabbedModal::process_event`). So the cost of building and laying out a page is paid on every tab switch, and the cost of
laying it out again on every change that dirties its layout. Changing the description text when a row is hovered or
focused is one of those changes.

## Page layouts

- **Columns** (General, Graphics, Games): `rush2::ui::OptionsPage` with `columns > 0` (`include/options_page.h`).
  Side-by-side columns that each scroll on their own, with the description across the bottom. Each option is one
  line, its name then its control: a switch (bool), a dropdown of its choices (enum), a slider (number) or a text box
  (string). `CompactOption` (`src/options_page.cpp`) follows the config's updates the same way the frontend's
  option elements do: value, disabled, hidden, enum details and disabled enum choices. Disabled choices use
  `recompui::Select::set_option_enabled`, a RecompFrontend patch addition. Headings, rows (`add_row`) and options go
  into the current column, and `next_column()` moves on. `create_ordered_tab` (`src/config.cpp`) takes the column
  count, and a `TabSection` with `new_column` starts the next column. A status line (`add_option_status`) goes in
  room the row keeps below its name and control, so the two stay centered on each other. Rows are 52 dp, a little
  taller than a select, so stacked selects don't touch.
- **Show/Hide lists** (General's AI Opponents and Circuit Courses): their lists open in the page's spare last column
  (`OptionsPage::expand_panel`, `rush2::ui::ExpandPanel`), one at a time, so opening one never makes its own column
  grow, scroll or move. The sections keep only their heading row in the second column.
- **Column order:** General: Gameplay, System | Circuits, Other (ghosts, Game Filter, AI Opponents), Run Setup Again | (the open list). A section with an empty heading adds no heading row. Graphics: Display |
  Quality, Effects | Textures, Your Own Upscales. Games: Rush 2049 | San Francisco Rush, Rush 2049 Arcade |
  California Speed (its options are named "CS ..." to fit).
- **List** (Controls, Online): `OptionsPage` without columns. Options are listed down the left and the description
  is on the right.
- **Cheats** (`src/cheats.cpp`) and **Sound** (`src/music.cpp`) build their own columns with the same placement.

## Setup

`src/setup.cpp` (`include/setup.h`). A fresh install (no `general.json` before the configs load) opens the setup screen
from the launcher once a valid ROM is stored, before the launch build pass (`launcher_update` in `src/main.cpp` waits on
`rush2::setup::pending()`). The General tab's **Run Setup Again** (the last row of its second column) closes the menu, opens the same screen,
and the menu opens again when it closes. It is a plain `recompui::Modal` in its own context, opened from a UI task
(`recompui::queue_ui_task`, so no other context is open). Its steps are rebuilt in the modal's Update event, never in a
button callback, which would delete the button it runs in.

- **Game Rules**: two cards side by side, Original and Enhanced, each one whole button. Each lists what it does for every setting whose two
  values differ (the `keep` and `change` texts in `rules()`), and has a button that picks it. Once one is picked (at once
  when opened from the menu), the rule settings show below in columns (`groups`) as ordinary options to turn on or
  off, with Ambient Occlusion and Car Sheen (`extras`: offered here, never changed by a preset). The card matching the settings is outlined; neither is when they match neither (Custom). Picking a card from
  Custom asks first. Next (header) goes on. On a first launch Escape or Start is Next too (the footer says so), so it
  can't skip the ROMs page; from the menu it closes.
- **Other Games (optional)**: the Games tab's page itself (`rush2::games::create_page`), with Back and Done.

**Presets.** The `rules()` table in `src/setup.cpp` lists every setting that changes how Rush 2 itself plays, with
its Original (Rush 2's) and Enhanced value. It also lists which other games' cars race on whose tracks (Original: each
game's on its own). The Rush 2049 and California Speed ones (`group` -1: Wings, Stunt Scoring, Odometer, Car Speeds,
the drone settings) aren't on the Game Rules step, which comes before those games are added. The preset still sets them,
and the Other Games step shows them. A fresh install starts on Enhanced's values. Settings that only change the other
games' content, the controls, the cameras or the graphics aren't in it. Settings are never split per game. The screen's
cards list the table's `keep`/`change` texts and the customize columns its settings, so both stay current. A new setting that
changes base Rush 2 goes into `rules()` with both values, its column, and, if its default differs from Rush 2's, a
short `keep` and `change` line for the cards.

**Profile.** It is kept in players.json's `setup` section: `profile` (`original`/`enhanced`), `known` (the rule
settings the profile has been applied to) and `pending` (a first launch that hasn't closed the screen). At startup
(`rush2::setup::load`, after every config is loaded), a rule setting missing from `known` takes the profile's value. So
an Original player's settings added in later versions start at Rush 2's behavior. An install from before this had no
section. It gets none of the screen: its profile is Original if its settings match Original, otherwise Enhanced, and its
settings are left as they are. Custom is shown, not stored: the settings match neither preset.

Testing: `tools/first_boot_test.py OUT_DIR [--rules original|enhanced]` goes through Locate ROM, the setup screen and
into the game in a sim's own data folder, then checks the stored profile and settings. In the menu,
`tools/menu_shot.py` with `ESC,DOWN,RIGHT` and 8 `DOWN` lands on Run Setup Again. Run it with `RUSH2_SIM=1 RUSH2_SIM_TURBO=0
RUSH2_SIM_SHOW=1 RUSH2_DATA_DIR=<that folder>` so it doesn't touch the player's settings. Sim runs without
`RUSH2_TEST_FIRST_BOOT` never show the screen.

## Layout cost

RmlUi 6.0 has no layout cache. A flex container lays out each item up to three times: to measure its base size, to
measure its cross size, then for real. A box that scrolls can repeat that once a scrollbar appears. Nested flex boxes
multiply all of it. The modal shell alone is five flex levels deep (page wrapper, modal, body, `ConfigPage`,
`ConfigBody`). The Sound page added five more around about 60 song rows, so every row was laid out hundreds of times.
Sound took 150 ms per layout and Cheats 44 ms. That stutter showed on every tab switch, and on Sound with every move
of the cursor.

Two fixes:

- **`LayoutMeasureCache`** (RmlUi patch, `Source/Core/Layout/LayoutDetails.*`, used in `FlexFormattingContext.cpp`
  and `LayoutEngine.cpp`). Within one layout of a document, a measurement (shrink-to-fit width, flex base height,
  flex cross height) of an element at the same input sizes gives the same answer, so it is made once and reused.
  Only measurements are cached. Every element is still formatted for real, so the boxes come out identical: the
  `--dump` output of the benchmark matches the unpatched library on the settings page mock-ups and on 12 random
  layouts (wrapping flex, floats, inline blocks, scroll boxes, percentages). It cuts layout 4-7x everywhere,
  including tabs this work didn't touch.
- **Placed, not flexed, pages.** The column pages, Cheats and Sound put their columns and description in place with
  `position: absolute` in a block page side, so no flex level sits between the shell and the rows. Each row is a
  single flex row, and Sound's sideways-scrolling strip is one flex row of block columns. Keep new pages that way:
  one flex level for a row is cheap, but a flex box of flex boxes of rows is not.

Benchmark numbers (`tools/rml_layout_bench`, the RML mock-ups in `tmp/rml_bench/gen.py`):

| Page | Before | Cache | Cache + placed layout |
| --- | --- | --- | --- |
| Sound | 150 ms | 23 ms | 4-9 ms |
| Cheats | 44 ms | 7 ms | 3 ms |
| General (old list) | 18 ms | 5.5 ms | (columns) 3 ms |

## Icons

Each `<svg>` element used to load, parse and rasterize its own file and upload its own texture. Upload in
recompui's renderer (`ui_renderer.cpp`, `create_texture`) submits a copy and waits for the GPU each time, so Sound's
play button for each song stalled once per song. `ElementSVG` (RmlUi patch) now shares the parsed file between every
element, and one texture between every element that shows the file at the same size. The textures stay until RmlUi
shuts down (`ElementSVG::ReleaseSharedResources`, from the SVG plugin's `OnShutdown`, while the render managers still
exist). Past 256 textures, the ones nothing shows any more are dropped when a new one is made. On the Sound mock-up,
68 icons now make 1 texture.

## Measuring

`tools/rml_layout_bench.cpp` (build with `cmd //c "tools\rml_layout_bench.bat"`, which links the RmlUi built in
`build/`). For each RML file it times load, the first layout, the mean of 20 relayouts forced by changing the text of
the element with id `desc`, and the first draw with the number of textures it made. `--dump FILE` writes every
element's box, to diff two RmlUi builds. Run it from the repo root (it loads `assets/InterVariable.ttf`). Mimic a
page's nesting in RML to compare structures before building one.
