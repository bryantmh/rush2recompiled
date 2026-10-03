# Rush 2 Recompiled

A static recompilation of **Rush 2: Extreme Racing USA** (N64, NTSC-U, `NR2E`) to native PC code using
[N64Recomp](https://github.com/N64Recomp/N64Recomp), [N64ModernRuntime](https://github.com/N64Recomp/N64ModernRuntime)
and the [RT64](https://github.com/rt64/rt64) renderer.

No game assets are included; you need your own ROM.

## Status

- Game code recompiles (1,596 functions across 3 code segments) and is playable through menus and races.
- Uses the standard N64: Recompiled launcher and config menu ([RecompFrontend](https://github.com/N64Recomp/RecompFrontend)).
- Aspect ratio (Settings > Graphics): **Original (4:3)**, **16:9**, or **Fill** (matches the window, any width).
  The 3D view is widened by extending each player view's scissor to the full framebuffer (hook in `us.toml`),
  which also removes the original top/bottom overscan border; 2D menus stay in the centered 4:3 area.
- HUD placement (Settings > Graphics): **Original (4:3)**, **16:9**, or **Edge** (window edges). Race HUD widgets in
  the left/right third of the screen get an RT64 rect alignment so they move outward, grouped with any widgets they
  touch so multi-part elements stay together; menus are untouched (`src/hud.cpp`, hooks in `us.toml`).
- High framerate (Settings > Graphics > Framerate = Display): the game runs at 30 fps and RT64 interpolates to
  the display's refresh rate. Every 3D matrix is tagged with a stable RT64 matrix group ID (scene graph node index,
  camera per view, world-space polygon slot), so identical cars and their wheels are never mixed up; IDs change on
  teleports and camera cuts so those snap instead of sweeping (`src/interpolation.cpp`, hooks in `us.toml`).
- Controller Pak saves (players and their records): controller 1 has an emulated Controller Pak stored as a standard
  32KB `.mpk` image in `%LOCALAPPDATA%\Rush2Recompiled\saves\rush2.n64.us.mpk` (same format as emulator `.mpk` files).
  Controllers 2-4 report a Rumble Pak. Controller 1 rumbles as well: a hook in the game's pak thread registers the
  port with the rumble code when its Controller Pak initializes (`src/pak.cpp`, `us.toml`).

Threading notes: game threads are cooperative under the runtime, so the four busy-wait loops on the pending
graphics task counter yield (hooks in `us.toml`), and `osStartThread`/`osStopThread` use libultra semantics
(`src/threads.cpp`) because the game stops/starts other threads around critical sections.

## How the ROM is laid out

| Segment | ROM | RAM | Notes |
|---|---|---|---|
| boot | `0x1000` (uncompressed) | `0x80000400` | libultra, libaudio, inflate routine; text ends at `0x80018800` |
| main | `0xAFD0C0` (raw deflate) | `0x800539E0` | game code, inflated by the boot thread; text to `0x800BCBB0`, bss to `0x80125C90` |
| ovl | `0xB3D20E` (raw deflate) | `0x803AA800` | dynamically loaded/unloaded by `func_800A62C0` |

Microcode: graphics is F3DEX2 (`F3DEX.NoN fifo 2.07`, text `0x800196F0`), handled by RT64.
Audio is libultra `aspMain` (text `0x800188D0`, data `0x8001C680`), recompiled with RSPRecomp.

Because the game decompresses its own code, `tools/extract.py` builds `rush2.us.recomp.z64`: the original ROM
with the code segments appended uncompressed at fake ROM addresses (`0x01000000`, `0x01080000`). The symbol file
places the sections there, and hooks in `us.toml` call `load_overlays` with those fake addresses right after the game
inflates each segment, so function pointer lookups resolve.

## Building (Windows)

Requirements: Visual Studio 2022+ Build Tools (C++), LLVM/clang-cl, CMake, Ninja, Python 3 with `spimdisasm`.
Submodule versions (runtime, RT64, RecompFrontend) match BanjoRecomp's, and the recompiler must be built from
`lib/N64ModernRuntime/N64Recomp` so its output matches the runtime's `recomp.h`.

```sh
# 1. Tools
git clone --recurse-submodules https://github.com/N64Recomp/N64Recomp ref/N64Recomp
git clone https://github.com/shygoo/n64sym ref/n64sym      # signature database for libultra naming
cmake -S lib/N64ModernRuntime/N64Recomp -B build-n64recomp -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build-n64recomp --target N64RecompCLI RSPRecomp   # copy the exes to .tools/

# 2. Prepare ROM images and symbols
py tools/extract.py "Rush 2 - Extreme Racing.n64"
py tools/gen_syms.py          # only needed if syms/ changes

# 3. Recompile
.tools/N64Recomp.exe us.toml
.tools/RSPRecomp.exe aspMain.us.toml

# 4. Build the executable
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_C_COMPILER=clang-cl -DCMAKE_CXX_COMPILER=clang-cl
cmake --build build
```

`tools/vsenv.bat <command>` runs a command inside the MSVC x64 developer environment.

## Running

```sh
build/Rush2Recompiled.exe [path/to/rom] [--show-console] [--autostart]
```

A ROM next to the executable (or in its parent folder) is imported automatically; otherwise use the launcher.
Settings and the ROM copy live in `%LOCALAPPDATA%\Rush2Recompiled`. Controls are configured in the launcher.
`--autostart` skips the launcher (useful for testing).
