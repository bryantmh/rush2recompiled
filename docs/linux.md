# Linux build

The Linux version is the same code built with clang against the system SDL2, packaged as an AppImage for any desktop
distro (glibc 2.38 or newer: Ubuntu 24.04, Fedora 39, Debian 13, current Arch and SteamOS). Nothing in it is specific to
the Steam Deck or SteamOS.

## Building

`tools/build_linux.sh` runs in WSL (or on Linux):

    wsl -d Ubuntu -- bash tools/build_linux.sh [--configure] [--no-sync] [--no-appimage] [--out DIR]

It copies the tree to `~/rush2-linux/src` (the Linux disk; building over `/mnt/c` is slow, and CMake's patch step must
not touch the Windows checkout's submodules), builds in `~/rush2-linux/build` with Ninja and packages
`tmp/linux/Rush2Recompiled-x86_64.AppImage` with linuxdeploy and appimagetool. The packages it needs are listed at the
top of the script. `RecompiledFuncs/` is the Windows side's N64Recomp output, copied as is.

- The Windows checkout has CRLF files and `lib/patches/*.patch` are LF, so the script makes the files the patches touch
  LF in the copy, keeping their times, or CMake's `git apply --reverse --check` fails.
- appimagetool quits silently, writing nothing, without `desktop-file-validate` (desktop-file-utils).
- linuxdeploy bundles SDL2, GTK (the file dialog), libpulse and their dependencies, but not glibc, the Vulkan loader,
  libGL, libdrm or ALSA's libasound (whose plugins must be the system's).

## Running from the AppImage

- `main()` changes to the executable's folder on Linux as on Windows (assets load relative to it). In an AppImage
  that is the read-only `usr/bin` of its mount.
- Settings, saves, mods and caches go to `~/.config/Rush2Recompiled` (librecomp's app folder). Portable mode can't
  work from an AppImage: the Data Location option reports that the folder isn't writable.
- The ROM is looked for next to the `.AppImage` file too (`$APPIMAGE`), as it's looked for next to the exe on Windows.

## Controller haptics

Rumble Mode > Enhanced's actuator haptics work on Linux as on Windows (include/dualsense_haptics.h,
include/steam_haptics.h). `src/linux_devices.cpp` maps a controller's SDL path to sysfs: a hidraw node when SDL's
HIDAPI driver has the controller, an evdev node when it doesn't (no udev access to hidraw), or a virtual evdev node
when Steam Input stands in for it. SDL 2.30 reports a Steam Input pad's real USB IDs and type
(`SDL_JoystickGetVendor` reads Steam's SteamVirtualGamepadInfo), so `src/input.cpp` routes it as the real controller,
and the haptics find the real device by those IDs, taking one no other player has.

- DualSense (USB): its sound card is the ALSA card on the same USB device. A 4-channel sink for the card (PulseAudio's
  or PipeWire's surround 4.0 profile, or UCM's Direct profile) is played through Pulse. alsa-ucm-conf's DualSense
  profile (upstream `USB-Audio/Sony/DualSense-PS5-HiFi.conf`, not in Ubuntu 24.04's 1.2.10) splits the card into speaker
  and headphone sinks over an ALSA dshare (`common/pcm/split.conf`: ipc_key 6678293, S16_LE, 4 channels, 20 ms period,
  500 ms buffer), so the haptics open a dshare with the same settings bound to channels 3 and 4, which joins it.
  Over Bluetooth there's no sound card.
- Steam Controller (2026) and Steam Deck: the same reports as on Windows, on the hidraw node: `write()` for the Steam
  Controller's output report 0x83, `HIDIOCSFEATURE` for the Deck's feature report 0xEA. Of a controller's hidraw nodes
  the Deck's is interface 2 and the Steam Controller's the one whose report descriptor has output report 0x83.
  Opening hidraw needs udev rules (Steam's steam-devices rules give the logged-in user Valve's and Sony's); without
  them the controller keeps SDL's rumble.

Untested on hardware so far: the log lines `[Haptics] Playing ...` and `[Haptics] Couldn't ...` say which path was taken.
