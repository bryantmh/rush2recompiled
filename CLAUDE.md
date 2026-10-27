Agents are expensive and I want token usage kept down. Default to doing the work yourself; think twice before spawning one and only do so when it clearly saves tokens (focused or repetitive tasks that can be fully specified in the prompt). Always set `model` explicitly to one tier below your own (Opus -> sonnet, Sonnet -> haiku), at medium effort, never inheriting the parent model.

We aren't british. no "colour" and the like

Ensure you document code and functions that you have taken the time to disassemble so that other sessions can easily reference and add to the knowledge base instead of working everything out again from scratch

Function knowledge lives in per-game registries: `syms/names/{rush1,rush2,rush2049}.toml` (addresses are per game, never comparable across games). Before disassembling anything, `python3 tools/names.py lookup <addr>` (or `search <text>`) to see if it is already known, including its Rush 2 counterpart via the `rush2` link. `python3 tools/rush2049/fsum.py <addr>` gives a compact summary of a Rush 2 function (args, callees with argument origins, struct fields, globals, strings, constants); start there instead of the full disassembly. After you work a function out, record it with `tools/names.py add <game> <addr> --desc ... --status verified|inferred [--name snake_case] [--rush2 <addr>] [--ported]` (the tool appends the address to form the symbol), and keep its `sig` (C prototype with named arguments) and `consts` (meaning of its magic numbers) up to date in the registry; `names.py import` applies a JSONL batch and `names.py index` regenerates the per-area tables in docs/functions/. Rush 2 global variables live in `syms/names/rush2_data.toml` (name, C type, desc; `lookup`/`search` cover them, `import --data` adds a batch, `fsum.py --gnext` summarizes their readers and writers); when you work out what a global holds, update its entry there. Long-form notes stay in `docs/`; the registry holds the facts that can be looked up. Every registry entry must carry a `ref` to the doc where it is explained; run `tools/names.py anchors` after adding or editing entries so each `ref` is a heading anchor (`docs/x.md#heading`), never a bare file, and make sure the doc actually mentions the address. `names.py check` fails on a docs `ref` without a valid heading anchor. Before trusting a Rush 2 <-> 2049 pairing, check it against the ROMs with `tools/rush2049/verify_names.py` (`rank`, `show`, `refs`, `best`; set RUSH2_ROM, RUSH2049_ROM, RUSH1_ROM); set `link` to `same-code` or `role`. `verify_names.py match --apply` auto-links Rush 1/2049 functions whose address-masked code equals a Rush 2 function's; rerun it after naming Rush 2 functions so the names propagate, then run `tools/names.py backlinks` to refresh the derived `rush1`/`rush2049` cross-link lists. Run `tools/names.py check` before committing. Names are applied to the build: `tools/gen_syms.py` emits `<name>_<ADDR>` for every named rush2 entry, and after naming a Rush 2 function you run `python3 tools/names.py rename --apply` (rewrites `syms/rush2.us.syms.toml`, `us.toml` and the code of `src/`, `include/`, `patches/`; comments and docs keep the old `func_XXXXXXXX`, which `names.py lookup` still resolves by address) and rebuild. Never hand-write a new `func_XXXXXXXX` for a function that has a registry name.

If porting from SF Rush or Rush 2049 default to porting the original games' code and just patching the Rush 2 differences rather than fully reimplementing from scratch

If your change will alter base Rush 2 behavior, it needs to go behind a toggle in the recomp UI

This is now a live app. Any behavior that would invalidate a config or save needs to be migrated at startup to the new system

Scratch scripts, logs and test output go in `tmp/` in the project root (gitignored) or the session scratchpad. Never in `build/`, the project root or anywhere else in the tree

Prefer reusable tools over one-off scripts. If a script would be useful again, put it in `tools/` with arguments and a comment on what it does, and extend an existing tool before writing a new one

Controller navigation must reach and show everything in a settings page. A row holding buttons in a scrolling page has to scroll itself into view when something in it takes focus: use `rush2::ui::FocusRow` or `OptionsPage::add_row()` (include/options_page.h), never a plain div. Give rows of buttons their own line so the buttons keep their full size.

Data the game derives from the ROMs at run time (converted tracks, built car decals and the like) must be cached on disk and reused, not rebuilt every boot. Key the cache on its inputs and the build so a new ROM or build rebuilds it once

RecompUI settings need to be hot-swappable or clearly labeled that they are not

Never assume the saved car or track is the current car or track. They change all the time. Set the ones you want manually

Do test runs outside the game when you can. When a run has to be in-game, pick JUST PLAY on Select Player, never a saved player, so the run doesn't touch the user's profile. The cursor starts on the first saved player, so the input script has to move it to JUST PLAY. Don't back up and restore build/saves around runs: a restore throws away whatever the user changed while the run was going
An ingame run takes a long time. Don't waste time doing one if the answer can easily be found outside the game, or wate time by doing runs piecemeal

Code reviews follow this standard so every review covers the same ground the same way. Reviews are independent on purpose: don't look for or use earlier reviews' findings
- Review the merged result: merge the branch with the current master in a scratch tree (tmp/ or the scratchpad, never the user's trees) and review that, so clashes with code master added since the branch started (RDRAM regions, hooks at the same address, renamed functions) show up
- Read every changed file in full, including us.toml hook entries, CMake and tools. Sampling isn't a review. End with a coverage list of every changed file marked read; a review with a file not read isn't done
- One bar: report only what a player would plausibly hit in normal use, with the trigger and what they'd see. Check each finding against the code before reporting it, and don't overstate likelihood or effect: a wrong or overstated finding is worse than none. Rare timing or hostile-input cases go in a short separate "unlikely" list, or are left out. "Nothing found" is a valid result. Line endings, formatting, merge conflicts and style are never findings
- Project rule breaks (base Rush 2 behavior changed without a toggle, unmigrated config or save changes, blocking the game thread) count as findings
- Fixing review findings: before handing back, review the fix diff to the same standard, since fixes are new code

If I've already given you a screenshot of an issue, you don't need to open the game just to reproduce the screenshot

The game's menus (game states, front-end screen ids, Start Game rows, track/car select, pause menu rows and confirms) and the input scripts that drive them are mapped in docs/menus.md. Read it before scripting a run through the menus, and add to it what you learn instead of working it out again

The race HUD's placement is the layout engine's rules (src/hud_layout.cpp, docs/hud_layout.md) over the HUD catalog (src/hud_catalog.cpp: every part the HUD can draw, from the ROM's tables and the game's callbacks). Never move or anchor a HUD element by hand in src/hud.cpp or a feature's code: add a new element's parts to the catalog (where it's drawn, where it can move, its role), say when it can appear (`possible()`, from the settings alone) and add its rule, then check every variant offline with tools/hud_preview (`--matrix` draws every layout, aspect and option with everything that can appear, from the catalog and ROM images, no capture needed) before any in-game run. `[hud] widget ...` lines in a game log mean the catalog is missing something

When running the game, turn the sound all the way down so it doesn't interrupt the computer's user

Stutters are unacceptable. If something you add or modify has a large frametime stutter, you have implemented it incorrectly. Adding a cache that still stutters on first load is not a valid fix.

A build is ready for me to test the moment it links: I watch and launch it without waiting for a message. So whatever a change sits behind (environment variables, settings, test switches, dev modes) is switched on in my own setup BEFORE the build starts, never after it, and you say what you switched on. A change I have to enable myself, or that I can't reach on a normal launch, isn't done