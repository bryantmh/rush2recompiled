# Rush 2 Recompiled

Rush 2: Extreme Racing USA, the 1998 N64 racer, running natively on your PC with widescreen and ultrawide support, high framerates, sharp text, and improved controller support

You need to bring your own copy of the game. Nothing from the original cartridge is included here.

## What you get

- **Widescreen.** Play in 4:3 like the original, 16:9, or expand the view to fill whatever window size you like.
- **Smooth framerate.** The game still runs at its original speed of 30fps but interpolated up to any arbitrary framerate, including a variable refresh rate mode that follows your display
- **HUD where you want it.** Keep the speedometer and lap info in the classic 4:3 spot, or push it out to the edges of the screen.
- **No pop-in.** Turn off level of detail and every car and building is drawn at full quality, no matter how far away it is.
- **Crisp fonts.** All the menu text and HUD numbers have been redrawn so they stay sharp at any resolution. You can switch back to the original blurry ones if you miss them.
- **Upscaled textures (optional).** Turn on Texture Upscaling in the graphics settings and the cars and tracks get smoothed at 2x or 4x with HQ2x/HQ4x, the filters emulators use for texture enhancement. The HUD and menus are left alone, and each car paint job reuses one upscale. Prefer another upscaler, like Topaz Gigapixel? Dump the textures with one click, upscale them with it, and install the results as your own texture pack.
- **Saves that just work.** Your players and records are kept on a virtual Controller Pak using the save file uses the same format as emulators.
- **Rumble.** Supported on any controller that can rumble and at the same time as saves
- **Two players, any controllers.** Xbox, PlayStation and most other controllers work, plus keyboard. Pick which controller belongs to which player and the game remembers it next time.
- **Rebind everything.** The game's own Controls screen now lets you map any button, trigger, stick or key. It shows the right button icons for your controller.
- **Split screen.** Two player races can run side by side in addition to the original top and bottom.
- **Three and four players.** After player 2, press START on any free controller to join.
- **AI opponents.** Choose the car, paint and rims of each computer opponent in the Players tab.
- **Ghost races.** Previously a 2049 exclusive feature. Record your runs and race against your own ghost car.
- **Unlocks.** Earn cars, tracks and engines through an UNLOCKS shop. You can disable and revert to original Rush 2 behavior if you wish

### Rush 2049 extras

If you also own **San Francisco Rush 2049** for the N64 (US version), you can point the game at that ROM and unlock a couple of additional features:

- **Wings.** Hold the Wings button when you're airborne to pop out wings and glide. They handle just like they did in 2049.
- **The 2049 tracks.** All six race tracks from Rush 2049 show up in the track select
- **The 2049 cars.** All thirteen Rush 2049 cars join the car select after Rush 2's own.
- **The 2049 music.** Each of the six 2049 tracks plays its own Rush 2049 song.
- **The 2049 stunt arenas.** The stunt arenas are playable from the track select.
- **Battle arenas.** Eight deathmatch arenas in the BATTLE row, with weapons, pickups, and arrows pointing to the other cars. Weapons can also be turned on in regular arena races with a cheat.
- **Coins.** Collect 2049's coins, which are counted per player in the Progress tab and will later be used for unlocks.

### San Francisco Rush extras

If you also own **San Francisco Rush: Extreme Racing** for the N64 (US version), you can point the game at that ROM on the Games tab and unlock:

- **The Rush 1 tracks.** All seven race tracks from the original San Francisco Rush
- **The Rush 1 music.** The nine Rush 1 race songs play on its tracks, picked at random.
- **Rush 1 car decals.** Some of Rush 1's own unique paint jobs are available as a stripe option.
- **Keys.** Collect the keys hidden on the Rush 1 tracks, which are counted per player in the Progress tab and will later be used for unlocks.


## What you need

- A Windows 10 or 11 PC
- A graphics card that supports DirectX 12 or Vulkan (anything from the last several years should be fine)
- Your own ROM of **Rush 2: Extreme Racing USA** (North American version). `.z64`, `.n64` and `.v64` files all work.
- Optional: a ROM of **San Francisco Rush 2049** (North American version) for the Rush 2049 extras
- Optional: a ROM of **San Francisco Rush: Extreme Racing** (North American version) for the Rush 1 tracks

## Getting started

1. Put `Rush2Recompiled.exe` somewhere you like.
2. Run it.
3. The first time, it'll ask you to find your Rush 2 ROM. Point it at the file and you're in.

After that the game starts right away. It keeps its own copy of the ROM, so you can move or delete the original file if you want.

Shortcut: if you drop the ROM in the same folder as the exe, it'll find it on its own and skip the question.

## Setting things up

Most options live in the settings menu (press Escape, or select).

- **General:** rumble strength, stick deadzone, steering and reverse options, background input, and data location
- **Graphics:** window mode, resolution, aspect ratio, framerate, anti-aliasing, level of detail, fonts and texture upscaling
- **Sound:** race music per game, plus whether other cars' engines are heard
- **Players:** choose which controller (or the keyboard) each player uses. Leave it on Auto and whoever presses a button first gets that player.
- **Progress:** see the keys and coins you've collected
- **Games:** pick your Rush 2049 and San Francisco Rush ROMs, then turn on their tracks, cars and Wings
- **Cheats:** unlock all tracks and cars, plus driving, survival, race and visual cheats. Enable the cheat menu first.

To change your button layout, use the game's own **Controls** screen, under Setup or from the pause menu. Pick a row, press A, then press the button you want for it.

## Where your stuff is saved

Settings and saves live in `%LOCALAPPDATA%\Rush2Recompiled`. Paste that into the File Explorer address bar to get there.

Your saves are in the `saves` folder: `rush2.n64.us.mpk` (the game's own save), `rush2.n64.us.json` (records, cars, keys, coins and unlocks of the added games) and `ghosts`. Back the folder up if you care about your records.

Texture upscaling keeps its files in the `texture_upscale` folder there: the upscaled textures it has made (so nothing is upscaled twice), and the `dump` and `upscaled` folders for your own upscales.

Want to keep everything on a USB stick or next to the game? Turn on portable mode in the General tab under Data Location. Your data moves into the game's folder the next time you launch.

## Credits

Built with [N64Recomp](https://github.com/N64Recomp/N64Recomp), [N64ModernRuntime](https://github.com/N64Recomp/N64ModernRuntime), [RecompFrontend](https://github.com/N64Recomp/RecompFrontend) and the [RT64](https://github.com/rt64/rt64) renderer. Texture upscaling uses [hqx](https://github.com/grom358/hqx) (LGPL 2.1, in `lib/hqx`). Big thanks to everyone behind those projects.

Track banner images for the San Francisco Rush tracks and Rush 2049's Metro and Presidio by Bunny.

Rush 2 and Rush 2049 belong to their respective owners. This project isn't affiliated with them and doesn't include any of their game data.

Curious how it works or want to build it yourself? See [docs/BUILDING.md](docs/BUILDING.md).
