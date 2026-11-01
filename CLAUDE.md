# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

An unofficial Android (arm64) port of the original Xbox game *Mortal Kombat: Shaolin Monks* (title 4D570029), built from the Android host of [Black-Android](https://github.com/RanggaCasper/Black-Android) and made with the [XboxXBE-Recomp](https://github.com/TGP482/XboxXBE-Recomp) SDK. The game's x86 code in `default.xbe` is statically recompiled to C++ on a Windows PC. That C++ is then built for Android together with the SDK's portable runtime and this repo's host code. Drawing goes through Vulkan 1.1, and windowing, input and audio through SDL3. The repo ships no game assets. `assets/`, `iso/` and `generated/` hold the user's own files or files derived from them, and all three are gitignored.

## Build

Everything runs on Windows. Requirements: Visual Studio 2022 C++ tools, plus Android SDK Platform 34, NDK `28.2.13676358` and CMake `3.31.6`. Java 17–21 is needed for Gradle 8.7 (`.vscode/settings.json` points to JDK 17). Python is used only to pull the xbe out of an ISO.

- `build.bat` runs the full pipeline:
  1. Checks the tools.
  2. Gets `assets/default.xbe`. If it is missing, `python -I tools/xdvdfs.py <iso> default.xbe assets/default.xbe` extracts it from `assets/*.iso` or `iso/*.iso`.
  3. Builds the recompiler (`tools/CMakeLists.txt` → `out/build/android-tools/xbe_recomp.exe` and `xbe_symbols.exe`) with clang-cl.
  4. Writes the `[symbols]` table into `config/game.toml` with `xbe_symbols`, only if the file doesn't have one yet.
  5. Runs `xbe_recomp config/game.toml --xdk <sdk>/xdk` → `generated/game/`.
  6. Runs `gradlew.bat assembleRelease`.
- To rebuild only the APK after editing host code (no re-recompile): `gradlew.bat assembleRelease`. Output: `app/build/outputs/apk/release/app-release.apk`, signed with the debug key.
- After editing `config/game.toml` (hooks, symbols, ABI, partitions), the recompile step has to run again. Run `build.bat`, or run the two `out/build/android-tools` commands by hand.
- `gradlew.bat assembleDebug` still builds the native code as Release: unoptimized recompiled code is too slow to run.
- Install: `adb install -r app/build/outputs/apk/release/app-release.apk`. On the device, settings (`shaolinmonks.toml`, `android.toml`, `layout.toml`), saves (`hdd`), logs and the shader cache are in `Android/data/com.racas.shaolinmonks/files`.
- The first native build compiles 128 partitions of recompiled code and takes 30–90 minutes.
- `-PxbeGenerated=<dir> -PxbeImage=<file>` makes Gradle build recompiled code from somewhere other than `generated/game`.

There are no tests or linters in this repo.

## CI/CD

`.github/workflows/build.yml` runs on Windows for each `v*` tag, and by hand (workflow_dispatch, artifact only). It decrypts `ci/default.xbe.enc` with the `XBE_KEY` secret (AES-256-CBC, PBKDF2, 200000 iterations; the command to make it is in the workflow's header), checks its SHA-1, runs `build.bat` with `GRADLE_ARGS` setting `-PversionCode`/`-PversionName` from the tag, and for a tag publishes a release with `ci/release-notes.md` and the commits since the previous tag. `app/build.gradle` signs with a release key from the `BLACK_KEYSTORE*` environment variables when they are set (the workflow sets them from the `KEYSTORE_BASE64`, `KEYSTORE_PASSWORD`, `KEY_ALIAS` and `KEY_PASSWORD` repository secrets), else the debug key. Releases since v1.0.0 are signed with the keystore in `.local/` (gitignored); a different key breaks every installed copy's updates. `UpdateChecker.REPO` names the repository whose latest release the app checks.

## Where the SDK comes from

The SDK is vendored in `sdk/` (upstream commit in `sdk/UPSTREAM_COMMIT`), changed in one place: `ExQueryNonVolatileSetting` takes XC_LANGUAGE from a weak `app::ConsoleLanguage()`, which `src/game/game.cpp` defines. Both `CMakeLists.txt` files look for the SDK in this order: `sdk/` in the repo, then `../XboxXBE-Recomp` beside the repo. If neither exists, they FetchContent the SDK's `main` branch. A sibling checkout lets you read or change the runtime (`<sdk>/runtime/{app,core,kernel,xdk,gpu,audio}`), whose headers (`app/app.h`, `kernel/kernel.h`, `xdk/hle.h`, `core/input.h`, ...) the host code includes. Gradle's `fetchSDL` task shallow-clones SDL `release-3.4.16` into `third_party/SDL`. Its Java sources go into the APK and its C side becomes `libSDL3.so`. glslang, toml++ and imgui are fetched by CMake.

## Architecture

The root `CMakeLists.txt` builds one shared library, `libmain.so`, which SDL's activity loads. It is made from:

- **`generated/game/`**: recompiler output (`x86_recomp.N.cpp` for N < `partitions`, `x86_func_mapping.cpp`, `image_symbols.h`). Compiled with `-O2 -w`. Never edit it by hand. Change `config/game.toml` or the host code and recompile instead.
- **The SDK runtime** (`<sdk>/runtime/*/*.cpp`), without the Windows-only parts and the parts this repo replaces. The regex filter in `CMakeLists.txt` drops `app/`, `core/input.cpp`, `gpu/d3d12`, `gpu/render_thread`, `audio/xaudio2`, `xdk/dsound` and `xdk/d3d8`.
- **`src/`**, the game-specific host, organized as `src/<area>/*.cpp` and globbed by CMake:
  - `src/game/`: game hooks and patches. `game.cpp` has the language glue and `FrameSkip` (hook at 0x0CB800): gameplay steps a fixed 1/60 s a frame, so frames behind the clock set the game's own PAL 50 Hz skip-draw flag (0x5CAB98) to keep its speed under 60 fps (FRAME SKIP setting), and `WidenAspect` (hook at 0x12432E, ULTRAWIDE setting, off by default): the cameras' projection aspect is the screen's 640/480 always, so a camera of about 4:3 (1.2 in gameplay) gets the shown frame's aspect (Hor+). `native/vulkan.cpp`'s `Flatten` then draws the 2D (XYZRHW, and programs whose oPos.w is 0 or 1) 4:3 in the middle of a wider frame, except full width draws and programs reading a target. Black's hooks were all tied to Black's addresses and were dropped.
  - `src/d3d/`: the title's Direct3D 8 (XDK 5849) entry points. `Direct3D_CreateDevice` also sets the device fields the library's init would (FVF shader, vertical blank event, display flags) and starts the vertical blank thread, which Black did from a hook. `render_thread.cpp` records D3D calls on the game's render thread and replays them on a dedicated Vulkan thread. Vertex, texture and surface memory is read at draw time, the way the console GPU reads it, so XDK fences, busy checks and locks wait on that replay.
  - `src/sound/`: DirectSound reimplemented (buffers, streams, ADPCM decode, mixer, reverb).
  - `src/common/`: `game_info.cpp` defines `app::kGame`, the struct that describes the game to the runtime (title, settings file, option bindings, load/save/init, pad and rumble filters, UI callback). Also here: the ImGui settings panel (`menu.cpp`, opened by the touch gear or F10), `options.h` (`game::Options`, saved to `shaolinmonks.toml`; most fields are Black's and unused) and translations.
- **`native/`**, the Android host. `main.cpp` runs SDL's window and event loop, the app lifecycle and foreground/surface handling. `vulkan.cpp` is the Vulkan backend, and `vk_shader.cpp` compiles the SDK's HLSL shader translations to SPIR-V with glslang. The rest: `touch.cpp` (touch controls and layout editor, plus keyboard and mouse), `gyro.cpp`, `input.cpp` (controllers, and the settings panel's CONTROLLER page bindings, saved as `[pad]` in `shaolinmonks.toml`, laid over SDL's mapping), `audio.cpp`, `ui.cpp` (ImGui), and `game_files.cpp`, which finds the game files or ISO and checks the xbe's SHA1 against `kImageSha1`. `host.h` declares the interfaces these parts share. `native/compat/crt.h` is force-included into all host sources (not the generated ones).
- **`app/`**: Gradle module. `ShaolinMonksActivity.java` (package `com.racas.shaolinmonks`) extends `SDLActivity`. It asks for all-files access (the storage permission on Android 10) and creates `/storage/emulated/0/ShaolinMonks` and `files/assets`, which are where the game files are looked for, along with a `ShaolinMonks` folder at the top of each SD card. `UpdateChecker.java` is off while its `REPO` is empty.

### Netplay

`native/netplay.cpp` is co-op over the network by lockstep: both phones run the game and trade only the pads (UDP, TCP on the same port for the host's `hdd` saves). `game::Pad` (`src/common/game_info.cpp`) ends in `netplay::Filter`, which takes port 0 as the phone's own pad and, while a session plays, replaces ports 0 and 1 with the traded ones (host is player 1, guest player 2). `netplay::Step()` runs at the top of the `FrameSkip` hook (0x0CB800, once per gameplay frame), waits for the friend's pad of that frame and latches both; a gameplay gap (no frame for 1 s without a focus pause) ends the epoch, and both sides meet again at a barrier that also copies `netplay_sync` guest memory from the host. Every 60 frames the host's state words (`netplay_sync` plus what the host's scan of all guest memory found to be a random number generator state: three looks that fit one LCG/xorshift/Park-Miller recurrence; the guest writes a word only after seeing it move like that generator itself) ride on the input packets and the guest applies them at the same frame. At each barrier the guest also takes the host's differing pages of the image's data, the heap and the physical memory over TCP (`FetchState`/`ServeState`, `netplay_state_copy`), keeping its own handles and dispatcher-object headers; the host stays at its barrier until it is served. The state hash and its `DESYNC` log show where the two games part, and so does the memory comparison (`netplay_diag`: at frames 2, 62 and 302 of an epoch each phone keeps its non-zero committed pages, the guest fetches the host's by TCP, `ServeTcp` telling that from a save fetch by the first bytes, and logs the words that differ). The panel's NETPLAY page (`src/common/menu.cpp`) drives it. `tools/netplay_loopback_test.cpp` tests the link on a PC (build line in its header).

### How recompiled code reaches the host

`config/game.toml` is the recompiler's input:
- `[xbe]`: path, entry point, XDK version, output dir, `partitions` (CMake reads this number to list the generated sources), and code sections.
- `[symbols]` / `[data]`: addresses of library functions and globals in the image, 0 where the image has none. Host code refers to them through `SYM(Name)` from `image_symbols.h`.
- `[abi]`: register-argument conventions for link-time-code-generated XDK functions.
- `[[hook]]`: entries `at = <x86 address>`, `call = "<Name>"`. The recompiler emits a call at that address to a host function `void Name(X86Context& ctx, uint8_t* base)`, defined in `src/game/`. To add a hook, add the `[[hook]]` entry, define the function, and recompile.

Host code reads and writes guest memory through the kernel helpers (`HostPtr(va)`, `R32`, ...) and guest registers through `X86Context`.

### Supported game versions

The USA and Europe releases are one disc with one `default.xbe` (SHA-1 `20e283c44fdb6062a760e4732d765036bac18067`). The disc has two more executables the game launches with `XLaunchNewImage`: `jarvos.xbe -game mk2` (the unlockable MK II, Midway's arcade emulator) and `Suffering2_Gold.xbe` (a demo). They are not recompiled and the Android host cannot switch images, so `HalReturnToFirmware` shuts the app down when either is picked.

### Notes on this image

- No `[abi]` entries: unlike Black, the D3D library here is not link-time code generated; its functions take their arguments on the stack.
- Resolved by hand in `config/game.toml`: `CDirectSoundStream_AddRef`/`Release` (0x3FBC3C/0x3FBC83, the ones in the stream vtable 0x4C2828), `D3DDevice_FlushVertexCache` (0x3CB140 pushes NV097 0x1710, its twin 0x3CB360 a NOP) and `D3D_FvfShader` (0x3F32B8, loaded by `D3DDevice_SetVertexShader` 0x3D0337).
- The touch controls (`native/touch.cpp`) are a whole Xbox pad: no game hook tells menus from play, so `input::Play()` is always true, and a drag on the right is the right stick.

## Conventions

- Native code is C++20 and is compiled with `-fsigned-char -fno-strict-aliasing`, matching x86 semantics.
- Only arm64-v8a is built, minSdk 29 (`native/compat/crt.h` supplies `memfd_create`, which bionic has from API 30). Libraries are linked with 16 KB page alignment.
- Comment style is terse, lowercase, and explains *why*, as in the existing files.
