Agents are expensive and I want token usage kept down. Default to doing the work yourself; think twice before spawning one and only do so when it clearly saves tokens (focused or repetitive tasks that can be fully specified in the prompt). Rarely more than 2. Always set `model` explicitly to one tier below your own (Opus -> sonnet, Sonnet -> haiku), at medium effort, never inheriting the parent model.

We aren't british. no "colour" and the like

Ensure you document code and functions that you have taken the time to disassemble so that other sessions can easily reference and add to the knowledge base instead of working everything out again from scratch

Function knowledge lives in per-game registries: `syms/names/{rush1,rush2,rush2049}.toml` (addresses are per game, never comparable across games). Before disassembling anything, `python3 tools/names.py lookup <addr>` (or `search <text>`) to see if it is already known, including its Rush 2 counterpart via the `rush2` link. After you work a function out, record it with `tools/names.py add <game> <addr> --desc ... --status verified|inferred [--name snake_case] [--rush2 <addr>] [--ported]` (name only when sure; the tool appends the address to form the symbol). Long-form notes stay in `docs/`; the registry holds the facts that can be looked up. Every registry entry must carry a `ref` to the doc where it is explained; run `tools/names.py anchors` after adding or editing entries so each `ref` is a heading anchor (`docs/x.md#heading`), never a bare file, and make sure the doc actually mentions the address. `names.py check` fails on a docs `ref` without a valid heading anchor. Before trusting a Rush 2 <-> 2049 pairing, check it against the ROMs with `tools/rush2049/verify_names.py` (`rank`, `show`, `refs`, `best`; set RUSH2_ROM, RUSH2049_ROM, RUSH1_ROM); set `link` to `same-code` or `role`. `verify_names.py match --apply` auto-links Rush 1/2049 functions whose address-masked code equals a Rush 2 function's; rerun it after naming Rush 2 functions so the names propagate, then run `tools/names.py backlinks` to refresh the derived `rush1`/`rush2049` cross-link lists. Run `tools/names.py check` before committing. Still to do: have `tools/gen_syms.py` emit the registry names and rename `func_XXXXXXXX` references in `us.toml`/`src` in one verified change; until then, registry names are not yet applied to the build.

If porting from SF Rush or Rush 2049 default to porting the original games' code and just patching the Rush 2 differences rather than fully reimplementing from scratch

If your change will alter base Rush 2 behavior, it needs to go behind a toggle in the recomp UI

This is now a live app. Any behavior that would invalidate a config or save needs to be migrated at startup to the new system

Scratch scripts, logs and test output go in `tmp/` in the project root (gitignored) or the session scratchpad. Never in `build/`, the project root or anywhere else in the tree

Prefer reusable tools over one-off scripts. If a script would be useful again, put it in `tools/` with arguments and a comment on what it does, and extend an existing tool before writing a new one
