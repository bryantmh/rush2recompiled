# macOS build

The macOS version is built by GitHub Actions on an Apple Silicon runner (`.github/workflows/macos.yml`), not on the
Windows PC: Apple's SDK and Metal shader compiler only run on a Mac. Each build's `.dmg` is kept as a workflow artifact
(visible to the repo's collaborators only).

## Releases

Every release gets its `.dmg` without anyone doing anything:

1. `tools/release.py` (the master pre-push hook) builds the release on the PC, then uploads that build's ROM-derived
   sources with `mac_ci_sources.upload(.release/src)`; it skips the upload when the pack for those inputs is there.
2. It pushes master and creates the release. The master push starts the workflow.
3. When the build is done, the workflow attaches `Rush2Recompiled-<version>-macOS.dmg` to the newest release whose game
   code equals the build's (`git diff <tag> HEAD` ignoring `.github`, `tools` and `docs`), waiting up to 3 minutes for
   the release to appear. That's about 20 minutes after the release goes up; the release notes say the `.dmg` follows.

The trigger is the master push, not the release event: GitHub runs a release event's workflow from the tag's commit, so
a fix to the workflow could never reach an existing release. A `--no-verify` master push of tool-only changes (no
release) therefore attaches its build to the current release, which has the same game code. A failed Mac build doesn't
hold back the release; GitHub emails the failure. The PC's `gh` token can't start workflows by hand (no `actions:
write`), so a missing `.dmg` is fixed by pushing master again with `--no-verify`, or from the Actions page with the
release tag as input.

## Getting a test build

1. On the PC with the ROM, regenerate the ROM-derived sources for the commit to build (`./.tools/N64Recomp.exe us.toml`,
   `./.tools/RSPRecomp.exe aspMain.us.toml`), then `py tools/mac_ci_sources.py upload`. This attaches them to the draft
   release `ci-recomp-sources`, keyed on a hash of the recompilers' inputs. Only needed when `us.toml`, the symbol file
   or `aspMain.us.toml` changed.
2. Push the branch (or run the workflow by hand). The workflow downloads the matching sources, runs
   `tools/build_macos.sh` and uploads `Rush2Recompiled-macOS.dmg` as the run's artifact.
3. Download the artifact from the run's page (`gh run download <run id>`), open the .dmg and drag the app to
   Applications.

The app is signed ad hoc (no Apple Developer ID, no notarization), so macOS quarantines a downloaded copy and says it
"could not verify" the app. Clear it once: open the app, then click Open Anyway in System Settings > Privacy & Security
(macOS 15 dropped the old right-click > Open way), or `xattr -dr com.apple.quarantine "/Applications/Rush 2
Recompiled.app"`. The release notes say this (tools/release.py). Only a paid Apple Developer ID with notarization
removes the warning.

## How the port works

- Rendering is RT64's Metal backend. `create_window` in `src/main.cpp` makes an SDL Metal view; the shaders are compiled
  HLSL -> SPIR-V (dxc-macos) -> Metal (spirv-cross, `xcrun metal`) at build time. Xcode 26 needs its Metal toolchain
  downloaded separately (`xcodebuild -downloadComponent MetalToolchain`).
- The bundle: `Contents/MacOS/Rush2Recompiled`, the UI `assets` and `recompcontrollerdb.txt` in `Contents/Resources`
  (the game makes that its working folder at startup, as the AppImage uses its `usr/bin`), and Homebrew's SDL2 and
  FreeType copied into `Contents/Frameworks` by dylibbundler, plus SDL3 as `libSDL3.dylib` (sdl2-compat loads it at
  run time from beside itself, which dylibbundler can't see).
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
- Homebrew's libraries are built for the runner's macOS (15), so the app needs macOS 15 or newer for now.
