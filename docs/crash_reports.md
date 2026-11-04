# Crash reports

How to read a crash from a player or a test run, and what earlier crashes turned out to be.

## Windows crash dumps

A crash leaves `%LOCALAPPDATA%\CrashDumps\Rush2Recompiled.exe.<pid>.dmp` (not for games launched from Git Bash, which
inherit MSYS's no-fault-dialog mode: launch test runs from PowerShell). Read it with
`cdb -z <dmp> -y build -c ".ecxr; kn 25; q"` (`C:\Program Files (x86)\Windows Kits\10\Debuggers\x64\cdb.exe`).

- The dump only matches the PDB of the build that wrote it. Read everything you need from it (globals with
  `x Rush2Recompiled!*name*`, then `dq`) before relinking `build/`, which replaces the PDB.
- Minidumps hold the threads' stacks and the exe's globals, not the native heap or RDRAM. The RDRAM base is in the
  `game_rdram` globals (`x Rush2Recompiled!*game_rdram*`).

## Display list faults

RT64's interpreter looks every command up in the table of the microcode it recognized (`Interpreter::hleGBI`). A
`G_LOAD_UCODE` (F3DEX2 `0xDD`) with microcode it doesn't know leaves that table null. Before the guard below, the next
command dereferenced it on the graphics thread (`RT64::Interpreter::processDisplayLists+0x87`, a read near address
`0x1E8`). A display list only does that when it has run into memory that isn't a display list.

The guard (lib/rt64 `hle/rt64_interpreter.cpp`, kept in `lib/patches/rt64.patch`) ends the task's list there and clears
the return stack, so the frame loses whatever was left to draw and the next task starts clean. Once per session it
writes to the data folder (`build/` for the portable build):

- `dl_fault.txt`: where the list was ended, the last 64 jumps the interpreter took (G_DL `DE`, G_ENDDL `DF`, a taken
  G_BRANCH_Z `04`, G_LOAD_UCODE `DD`, and `00` for each task's start) as RDRAM offsets, the return stack, RDPHALF_1
  and the segment table.
- `dl_fault_rdram.bin`: the first 16 MB of RDRAM (`0x80000000`-`0x81000000`), 32-bit words in host order (byte `a` of
  the N64 address space is at file offset `a ^ 3`).

Test: `py tools/test_race.py 13 --drones 7 --seconds 45 --data tmp/<dir> --env RUSH2_TEST_RACE_BATTLE=1 --env
R2_BATTLE_TEST=startrow,dlfault` makes the battle renderer (src/rush2049/battle_render.cpp) emit one G_LOAD_UCODE of
address 0 about 10 s into the race: the run logs `[RT64] Display list fault`, writes both files into `tmp/<dir>` and
keeps racing.

To find the culprit, take the last jump into the bad region from `dl_fault.txt` and read the list it came from in the
snapshot. Its address tells the owner: the spare RDRAM map in src/controls_menu.cpp lists who builds lists where.

### 2026-11-03: race battle on Haight 2049

A single-player race with the BATTLE row on, Haight 2049 (track 13), crashed right after the player ran over a weapon
pickup. The task was a normal F3DEX2 one (text `0x800196F0`, data `0x8001C940`, list `0x80255900`). When it crashed, the
interpreter was at `0x800491E8`, so the command at `0x800491E0` had been a `G_LOAD_UCODE` read from garbage.

`0x800491E0` is the game's decompression input buffer: `lzss_refill_buffer_80003B80` and
`inflate_refill_buffer_80005894` stream compressed ROM data through two 4 KB buffers, `0x800491E0` and `0x8004A1E0`,
and `inflate_state_a_80023390` holds the current one. So some display list called the start of that buffer. The dump
couldn't say which list: the RT64 return stack is on the heap. Ruled out offline:

- Rush 2049's weapon and pickup model files (61, 62, 63, 76): walking every model's lists from its OBHD record finds no
  G_BRANCH_Z, G_MTX, G_MOVEMEM or G_LOAD_UCODE and no segmented address, so battle_render's rebase covers them.
- The battle HUD's image decode (bounded by `image_end`).
- None of our code reads a global near `0x80023390`.

It didn't reproduce in 15 runs (`tools/test_race.py 13 --drones 7 --env RUSH2_TEST_RACE_BATTLE=1 --env
R2_BATTLE_TEST=log,fire,give<n>` for every pickup kind 0-10, and with `--user-settings` and a 2049 car). The next time it
happens, `dl_fault.txt` and the snapshot name the list.

## Menu navigation exception

`recompui::RecompNav::unwind_nav_in_direction` (lib/RecompFrontend `recompui/src/elements/ui_document.cpp`) threw
`std::runtime_error` when the focused element wasn't in its nav parent's children: a menu rebuilt or torn down while a
direction press was queued. It runs on RT64's present thread with nothing to catch it, so the game aborted (six test
runs on 2026-11-03 died at boot this way, on the launcher, from controller presses). It now ignores that press, and
`dive_nav_from_direction` no longer follows a null closest element.
