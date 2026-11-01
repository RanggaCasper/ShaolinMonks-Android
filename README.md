# Mortal Kombat: Shaolin Monks for Android
An unofficial native Android port of the original Xbox version of *Mortal Kombat: Shaolin Monks*, with touch controls, controller and keyboard support and extended graphics options.

The game's x86 code is statically recompiled ahead of time to C++ with [XboxXBE-Recomp](https://github.com/TGP482/XboxXBE-Recomp), then built for arm64. Drawing goes through Vulkan, and windowing, input and audio through SDL3. No emulator is involved. The Android host (Vulkan renderer, DirectSound, touch controls, settings panel) comes from [Black-Android](https://github.com/RanggaCasper/Black-Android).

[![Gameplay on a Xiaomi Pad 7 at 60 FPS](https://img.youtube.com/vi/_NjKztafI74/maxresdefault.jpg)](https://youtu.be/_NjKztafI74)

*Gameplay on a Xiaomi Pad 7 at 60 FPS ([watch on YouTube](https://youtu.be/_NjKztafI74)).*

> [!NOTE]
> This project does not include any game assets. You must provide the files from your own legally acquired copy of the game to install or build.

> [!WARNING]
> Work in progress. The port builds, but it has not been played through on a device yet. See [Status](#status).

## Installation
1. Install the APK.
2. Open **Shaolin Monks** and allow **all files access** when it asks (on Android 10, the **storage** permission).
3. Copy the game into `/storage/emulated/0/ShaolinMonks`, or into a `ShaolinMonks` folder at the top of your SD card: either the disc image (`.iso`) or `default.xbe` with the rest of the disc's files and folders (`GAMEDATA.WA*`, `front`, `English`, `classic`, ...). The app creates the folder on the phone's storage once access is allowed, and starts the game as soon as it finds the files.

If you don't grant that access, `Android/data/com.racas.shaolinmonks/files/assets` works too.

Settings (`shaolinmonks.toml`, `android.toml`, `layout.toml`), saves (`hdd`), logs and the shader cache are stored in `Android/data/com.racas.shaolinmonks/files`.

The disc is the same for the USA and Europe releases (one `default.xbe`, SHA-1 `20e283c44fdb6062a760e4732d765036bac18067`).

## Features
The settings panel opens from the gear at the top left of the screen (or F10 on a keyboard). Android's back button is the controller's Start, which pauses the game.

### Graphics
- **Render Resolution**: 20% to 200% of the screen's height, instead of the original 640x480.
- **V-Sync**, **Max Frame Rate** (unlimited, the display's refresh rate or a custom cap) and an **FPS Counter**.
- **Anisotropic Filtering**, applied to all textures.

### Touch Controls
A full Xbox controller on the screen:
- The left side of the screen is the **left stick**, wherever your thumb comes down. A drag on the right side of the screen is the **right stick**.
- **A**, **B**, **X**, **Y** in the controller's diamond and colours, **Black** and **White**, the **triggers**, **Start**, **Back** and a **D-pad**.
- **Controls Layout**: *Default* (triggers at the top corners) or *Low Triggers*, with the controls' opacity and overall size.
- **Edit Controls Position**: drag each control where you want it. To resize one, select it and tap **−** or **+**, or pinch it with a second finger.

### Other Input
- **Controller**: any controller SDL3 supports, with rumble. The touch controls hide while a controller is in use.
- **Keyboard**, with every controller button and both sticks rebindable: WASD the left stick, Space/K/L/J/I the face buttons, Q/E the triggers, the arrows the D-pad, Enter Start.

### General
- **Language**: English, French, German, Italian or Spanish. The game takes it from the console's language setting, which this sets; it applies at the next start. Indonesian translates the settings panel and plays the game in English.
- **Audio Output**: automatic, mono, stereo or 5.1 surround.
- **Background Pause**: the game and its sound pause while the app is in the background.

### Netplay (experimental)
Co-op over a direct network link, from the **NETPLAY** tab of the settings panel. Both phones run the game and trade only the controllers' input, one frame at a time (lockstep): the host plays player 1, the friend player 2. There is no relay server, so both phones need to reach each other by IP address (the same Wi-Fi, or a VPN).

1. Both install the same build and have the same game files.
2. The host opens the panel, **NETPLAY**, taps **HOST A GAME** and tells the friend the address shown there.
3. The friend types it in **HOST ADDRESS** and taps **JOIN A GAME**. With **COPY HOST SAVES ON JOIN** on, the host's saves replace the friend's `hdd` folder; the friend's own are kept in `hdd.netplay-backup`. Join before loading a game, so the game reads the copied saves.
4. Before gameplay both phones read the same two pads: the host's is player 1 and the friend's player 2, so the host drives the menus on both and the friend presses Start to join as player 2. Start the same co-op game (same save) and play. The first gameplay frame waits for the other phone, then both run together. The line at the top of the screen shows the state and the ping.

This only works while the two games stay identical, which needs the game's random numbers and clocks to match. Players in sync but monsters not means the games' random numbers differ. The host looks for the game's generator on its own (three looks at all of the console's memory from frame 240 of a level, for the usual recurrences) and copies it to the friend's game every second, once the friend's phone has seen the same word move like a generator too (a word of another meaning at that address is left alone).; the log says `random number generator found at ...` or how many words it scanned, and every 600 frames how often the friend's copy had to be corrected (often means something else than the generator differs). `netplay_auto_rng = false` turns the scan off. At frames 2, 62 and 302 of each level both phones keep a copy of the game's memory and the friend's phone asks the host's over the same port, then logs `diag ...` lines: how many pages differ (by kind: code, data, heap, contiguous, system, stacks) and the first words that do, host's value then its own; `netplay_diag = false` turns it off. When a level starts the friend's phone also takes the host's game memory (the pages of the game's data, heap and the console's physical memory that differ; not its own handles or kernel objects, not stacks), while the host waits at its start: the log says `state copy done` with the counts. `netplay_state_copy = false` turns that off. With `netplay_hash` set, a hash of the game's memory is traded every second and **OUT OF SYNC** shows (and `DESYNC` goes to the log) at the first difference. The log also has a line every 600 frames with how long this phone waited for the friend's input. Settings in `shaolinmonks.toml`:
- `netplay_sync = ["0x5C0000:16"]`: guest memory (`ADDRESS:LENGTH`, one packet's worth in all) copied from the host to the friend at each start of gameplay, for example the random number generator's state.
- `netplay_hash = ["0x5C0000:16"]`: what the state hash covers; with none, nothing is hashed and no desync is reported. `["auto"]` hashes the image's data and bss (0x4A0000 to 0x5CB000), which holds clocks and buffers that differ between phones anyway, so it reports false alarms; use it to find where the games part: on a `DESYNC` each phone logs the hash of every 4 KB page, and the pages that differ between the two logs are the place.
- `netplay_port`, `netplay_delay` (frames both phones wait on input, 1 to 12), `netplay_address`, `netplay_copy_save`.

Known limits: the touch controls and the host's rumble work, the friend's rumble does not; a menu or a load makes both phones leave gameplay and meet again at the next gameplay frame; the host accepts the first phone that asks. `tools/netplay_loopback_test.cpp` tests the link on a PC without the game.

## Status
- `xbe_symbols` finds 183 of the 238 XDK functions and 11 of the 12 data symbols the host knows; the ones it misses are the network stack (XNet, Winsock), Bink and the memory unit type, which the game doesn't use. Four more were resolved by hand in `config/game.toml`.
- **MK II and the bonus demo don't run.** The game plays the unlockable *Mortal Kombat II* by starting another program on the disc, `jarvos.xbe` (Midway's arcade emulator), and *The Suffering: Ties That Bind* demo with `Suffering2_Gold.xbe`. The Android host can't switch to another executable yet, so picking either one closes the app.
- Nothing game-specific is patched yet: no widescreen FOV, frame rate unlock or cheats. The game runs at its own frame rate and 4:3 picture, letterboxed.

Logs are in `Android/data/com.racas.shaolinmonks/files`. Please include the log when reporting a problem.

## System Requirements
- **Android** 10 or newer, arm64.
- **GPU** with a Vulkan 1.1 driver, for example Adreno 6xx or newer, Mali-G7x or newer, or Xclipse.
- **Memory**: 4 GB.
- **Storage**: 4 GB for the game files and saves.

## Building
Building requires a Windows PC.

### 1. Clone this repository

### 2. Add the game files
Put `default.xbe` in `assets/`, or put the disc image (`.iso`) in `assets/` or `iso/`. The build extracts `default.xbe` from the image.

### 3. Install the dependencies
- [Visual Studio 2022](https://visualstudio.microsoft.com/downloads/) with *Desktop development with C++*. The recompiler runs on the PC.
- [Android Studio](https://developer.android.com/studio). In its SDK Manager, install Android SDK Platform 34, NDK `28.2.13676358` and CMake `3.31.6`. Gradle needs Java 17 to 21 (`JAVA_HOME`).
- [Python](https://www.python.org/), needed only to extract `default.xbe` from a disc image.

### 4. Build
Run `build.bat`. It builds the recompiler, recompiles the xbe into `generated/game`, then builds `app/build/outputs/apk/release/app-release.apk` with Gradle. The first build compiles the whole game for arm64 and takes 30 to 90 minutes.

Install the APK with:
```
adb install -r app/build/outputs/apk/release/app-release.apk
```

The SDK is vendored in `sdk/`, at the upstream commit in `sdk/UPSTREAM_COMMIT`, with one change: the console's language comes from `app::ConsoleLanguage()` (`sdk/runtime/kernel/xboxkrnl.cpp`).

## Support
If you'd like to support the project, you can do so on [SociaBuzz](https://sociabuzz.com/racas/tribe).

## Credits
- [XboxXBE-Recomp](https://github.com/TGP482/XboxXBE-Recomp) by TGP482: the static recompiler and the portable runtime (Xbox kernel, XDK libraries and Direct3D 8) this port is built on.
- [Black-Android](https://github.com/RanggaCasper/Black-Android) by RanggaCasper: the Android host this port is built from.
- Zukiru: the HDR screen filter.
- [SDL](https://github.com/libsdl-org/SDL), [glslang](https://github.com/KhronosGroup/glslang), [toml++](https://github.com/marzer/tomlplusplus) and [Dear ImGui](https://github.com/ocornut/imgui).
- [xemu](https://github.com/xemu-project/xemu) and [Cxbx-Reloaded](https://github.com/Cxbx-Reloaded/Cxbx-Reloaded), which XboxXBE-Recomp used as references for Xbox hardware and API behavior.

*Mortal Kombat* is a trademark of Warner Bros. Entertainment Inc. This project is not affiliated with or endorsed by them.
