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
- **Column order:** General: Gameplay, System | Circuits, Ghosts, Opponents | (the open list). Graphics: Display |
  Quality, Effects | Textures, Your Own Upscales. Games: Rush 2049 | San Francisco Rush, Rush 2049 Arcade |
  California Speed (its options are named "CS ..." to fit).
- **List** (Controls, Online): `OptionsPage` without columns. Options are listed down the left and the description
  is on the right.
- **Cheats** (`src/cheats.cpp`) and **Sound** (`src/music.cpp`) build their own columns with the same placement.

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
