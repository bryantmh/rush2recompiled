# macOS build

The macOS version is built by GitHub Actions on an Apple Silicon runner (`.github/workflows/macos.yml`), not on the
Windows PC: Apple's SDK and Metal shader compiler only run on a Mac. Each build's `.dmg` is kept as a workflow artifact
(visible to the repo's collaborators only) so it can be tried on a Mac before it goes into a release.

## Getting a build

1. On the PC with the ROM, regenerate the ROM-derived sources for the commit to build (`./.tools/N64Recomp.exe us.toml`,
   `./.tools/RSPRecomp.exe aspMain.us.toml`), then `py tools/mac_ci_sources.py upload`. This attaches them to the draft
   release `ci-recomp-sources`, keyed on a hash of the recompilers' inputs. Only needed when `us.toml`, the symbol file
   or `aspMain.us.toml` changed.
2. Push the branch (or run the workflow by hand). The workflow downloads the matching sources, runs
   `tools/build_macos.sh` and uploads `Rush2Recompiled-macOS-<sha>.dmg`.
3. Download the artifact from the run's page (`gh run download <run id>`), open the .dmg and drag the app to
   Applications.

The app is signed ad hoc (no Apple Developer ID, no notarization), so macOS quarantines a downloaded copy. Clear it once
with right-click > Open, or `xattr -dr com.apple.quarantine "/Applications/Rush 2 Recompiled.app"`.

## How the port works

- Rendering is RT64's Metal backend. `create_window` in `src/main.cpp` makes an SDL Metal view; the shaders are compiled
  HLSL -> SPIR-V (dxc-macos) -> Metal (spirv-cross, `xcrun metal`) at build time. Xcode 26 needs its Metal toolchain
  downloaded separately (`xcodebuild -downloadComponent MetalToolchain`).
- The bundle: `Contents/MacOS/Rush2Recompiled`, the UI `assets` and `recompcontrollerdb.txt` in `Contents/Resources`
  (the game makes that its working folder at startup, as the AppImage uses its `usr/bin`), and Homebrew's SDL2 and
  FreeType copied into `Contents/Frameworks` by dylibbundler.
- Settings, saves and caches go to `~/Library/Application Support/Rush2Recompiled` (recompui's
  `get_app_folder_path`).
- `rush2::executable_path()` (`src/data_location.cpp`) finds the executable on every system; on macOS with
  `_NSGetExecutablePath`. The track and stripe caches key on it.
- Background workers lower their own priority with a QoS class (`src/prewarm.cpp`); `setpriority` would lower the
  whole process on macOS.
- DualSense and Steam Controller haptics, the GTK dialog fixes and gamescope touch input are Windows/Linux only and
  compile out.

## Not done yet

- Portable mode (`portable.txt`) writes into the read-only, signed bundle; on macOS it would have to go beside the app.
- No Developer ID signing or notarization, and no x86_64 (Intel Mac) build.
