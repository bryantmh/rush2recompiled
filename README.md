# Rush 2 Recompiled

Rush 2: Extreme Racing USA, the 1998 N64 racer, running natively on your PC with widescreen and ultrawide support, high framerates, sharp text, and improved controller support

You need to bring your own copy of the game. Nothing from the original cartridge is included here.

## What you get

- **Widescreen.** Play in 4:3 like the original, 16:9, or expand the view to fill whatever window size you like.
- **Smooth framerate.** The game still runs at its original speed of 30fps but interpolated up to any arbitrary framerate
- **HUD where you want it.** Keep the speedometer and lap info in the classic 4:3 spot, or push it out to the edges of the screen.
- **No pop-in.** Turn off level of detail and every car and building is drawn at full quality, no matter how far away it is.
- **Crisp fonts.** All the menu text and HUD numbers have been redrawn so they stay sharp at any resolution. You can switch back to the original blurry ones if you miss them.
- **Saves that just work.** Your players and records are kept on a virtual Controller Pak using the save file uses the same format as emulators.
- **Rumble.** Supported on any controller that can rumble and at the same time as saves
- **Two players, any controllers.** Xbox, PlayStation and most other controllers work, plus keyboard. Pick which controller belongs to which player and the game remembers it next time.
- **Rebind everything.** The game's own Controls screen now lets you map any button, trigger, stick or key. It shows the right button icons for your controller.

### Rush 2049 extras

If you also own **San Francisco Rush 2049** for the N64 (US version), you can point the game at that ROM and unlock a couple of additional features:

- **Wings.** Hold the Wings button when you're airborne to pop out wings and glide. They handle just like they did in 2049, flames and wind sound included, and each player can pick their own wing style.
- **The 2049 tracks.** All six race tracks from Rush 2049 show up in the track select, right after Rush 2's own tracks, each with a miniature of the real track. They come with their moving trains, trolleys, trap doors, boost pads and windmills, their blinking signs, their sounds, and Rush 2049's own music (switch the music off on the Rush 2049 tab to hear Rush 2's songs instead).


## What you need

- A Windows 10 or 11 PC
- A graphics card that supports DirectX 12 or Vulkan (anything from the last several years should be fine)
- Your own ROM of **Rush 2: Extreme Racing USA** (North American version). `.z64`, `.n64` and `.v64` files all work.
- Optional: a ROM of **San Francisco Rush 2049** (North American version) for the extras above

## Getting started

1. Put `Rush2Recompiled.exe` somewhere you like.
2. Run it.
3. The first time, it'll ask you to find your Rush 2 ROM. Point it at the file and you're in.

After that the game starts right away. It keeps its own copy of the ROM, so you can move or delete the original file if you want.

Shortcut: if you drop the ROM in the same folder as the exe, it'll find it on its own and skip the question.

## Setting things up

Most options live in the settings menu (press Escape, or select).

- **Graphics:** aspect ratio, HUD placement, framerate, level of detail and fonts
- **Players:** choose which controller (or the keyboard) each player uses. Leave it on Auto and whoever presses a button first gets that player.
- **Rush 2049:** pick your 2049 ROM, then turn on Wings and the extra tracks

To change your button layout, use the game's own **Controls** screen, under Setup or from the pause menu. Pick a row, press A, then press the button you want for it.

## Where your stuff is saved

Settings and saves live in `%LOCALAPPDATA%\Rush2Recompiled`. Paste that into the File Explorer address bar to get there.

Your save file is `saves\rush2.n64.us.mpk`. Back it up if you care about your records.

Want to keep everything on a USB stick or next to the game? Turn on portable mode in the General tab under Data Location. Your data moves into the game's folder the next time you launch.

## Credits

Built with [N64Recomp](https://github.com/N64Recomp/N64Recomp), [N64ModernRuntime](https://github.com/N64Recomp/N64ModernRuntime), [RecompFrontend](https://github.com/N64Recomp/RecompFrontend) and the [RT64](https://github.com/rt64/rt64) renderer. Big thanks to everyone behind those projects.

Rush 2 and Rush 2049 belong to their respective owners. This project isn't affiliated with them and doesn't include any of their game data.

Curious how it works or want to build it yourself? See [docs/BUILDING.md](docs/BUILDING.md).
