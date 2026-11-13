# Zombiewood for Windows (native port)

**English** · [Español](README.es.md)

A runtime that lets you play **Zombiewood 1.5.3** (an Android game by Gameloft) natively on Windows 10/11 x64. This
project is **only the execution environment**. It loads the game's own native library (`libZombieWood.so`, 32-bit ARM)
with a custom ELF loader, runs it through a JIT recompiler (dynarmic) and replaces Android with Windows: bionic
libc/pthread, OpenGL ES 2 (ANGLE on Direct3D 11), audio (SDL2), JNI, video (Media Foundation) and input (keyboard,
mouse, gamepad → virtual touches on the HUD).

> ## ⚠️ The game is not included
> This repository and the download package **contain no Zombiewood data at all** (no code, graphics, sound or videos).
> Every user must provide **their own legitimate copy** of the game; the built-in installer extracts what it needs from
> your APK. Game files (`*.apk`, `*.obb`, `*.so`, `data/`) are excluded in `.gitignore`, and the packaging script refuses
> to build a zip that contains them.

Status: playable at 60 fps (tutorial, movie menu and scene 1 "Body Count" fully completed, with saving and loading; see
[`docs/QA.md`](docs/QA.md) for what has and has not been tested: physical gamepads and scenes 2+ are untested).
Offline only: Gameloft's servers no longer exist, so the online store, TapJoy, etc. do not work.

> The in-game port menu (F1) is currently in Spanish; menu names are given below as *Spanish (English)*.

## Requirements

- Windows 10 or 11, 64-bit.
- A GPU with Direct3D 11 (the default backend); desktop OpenGL as an alternative.
- For videos: Windows Media Foundation (built into Windows; on **N/KN** editions install the *Media Feature Pack*,
  otherwise videos are skipped).
- Your copy of the game, version **1.5.3** (package `techj5gameszombiew153`):
  - the large APK (≈306 MB, with the OBBs inside at `assets/archiveObbCache.zip.001…`), **or**
  - the original APK + `main.1501.….obb` and `patch.15325.….obb`.
  Other versions may not work (the installer warns you if the SHA-256 hashes do not match).
- ~1 GB of free disk space for the installed data.

## Installation

1. Download `Zombiewood-Win64-<version>.zip` from the repository's **Releases** section
   ([latest version](https://github.com/ChrisDutary/Zombiewood/releases/latest)) and extract it into a folder you can
   write to (e.g. `C:\Games\Zombiewood`; **not** `Program Files`, and not inside the zip). It contains `zombiewood.exe`,
   `SDL2.dll`, `libEGL.dll`, `libGLESv2.dll`, `z.dll`, the Visual C++ runtime (`msvcp140.dll`, `vcruntime140.dll`,
   `vcruntime140_1.dll`), `gamecontrollerdb.txt`, `readme.txt`, `LICENSE.txt` and `licenses/`.
   Optional: check the SHA-256 published next to the zip (`Get-FileHash .\Zombiewood-Win64-<version>.zip`).
2. Run `zombiewood.exe`. The exe is not signed: if Windows SmartScreen shows *"Windows protected your PC"*, click
   **More info → Run anyway**. Since there is no data yet, the first-run wizard appears inside the window.
3. **Elegir APK… / Choose APK… (Enter)** → select your APK. It is streamed into `data/` (a few seconds on an SSD), the
   SHA-256 hashes are verified and `zombiewood.json` is written.
4. **Jugar / Play (Enter)**. You will see the logo, the intro (any key skips it) and the game.

Everything the port generates lives next to the exe (or in `ZOMBIEWOOD_HOME` if you set that environment variable):
`data/`, `saves/`, `saves_backup/`, `logs/`, `screens/`, `zombiewood.json`. To uninstall, delete the folder.

Headless: `zombiewood --install=<apk> [--obb=<main.obb> --obb=<patch.obb>] [--install-only]`; `--reinstall` reopens the wizard.

### Updating without losing your progress

Close the game and copy the **contents** of the new zip's `Zombiewood-Win64` folder over your game folder (*Replace the
files in the destination*). The zip only contains the program: `saves/`, `data/` and `zombiewood.json` are never touched.

### Moving your save to another PC

Install the port and your APK on the new PC, launch it once, close it, and copy the contents of your old `saves/` folder
into the new one. Saves are not tied to the machine.

## Default controls

The game only understands touches and the Android BACK/MENU keys; the port translates keyboard and gamepad input into
virtual touches.

| Action | Keyboard / mouse | Gamepad |
|---|---|---|
| Tap (menus, buttons) | left click | — |
| Move | `W A S D` | left stick |
| Aim and shoot | arrow keys, or hold right click | right stick |
| Medkit / weapon / pause / accept | `E` · `Q` · `P` · `Space` | `Y` · `RB`/`X` · `Start` · `A` |
| Back / game menu | `Esc` · `Tab` | `Back`/`B` · `Guide` |
| Port menu · screenshot · fullscreen | `F1` · `F12` · `F11` / `Alt+Enter` | — |

Everything can be remapped from **F1 → Controles (Controls)** (*Cambiar* / Change, then press a key or button; *+* adds
a second binding). `gamecontrollerdb.txt` includes SDL mappings for Windows gamepads; you can add your own.

**Gamepads (0.2.2+):** the *Mando* (Gamepad) section of F1 → Controles shows the connected controller, a live tester and
PlayStation button names. Use **Asistente: mapear con el mando** (gamepad mapping wizard) to assign each action by
pressing it on the controller, and *Intercambiar sticks* to swap which stick moves and which aims. If the controller is
listed but nothing reacts, close Steam/DS4Windows (they can take exclusive control of PlayStation pads) or set
*Driver de mandos* to *Compatibilidad* and restart.

## Configuration

The **F1** menu (Zombiewood · Gráficos/Graphics · Audio · Controles/Controls · Carpetas/Folders · Ayuda/Help) saves
instantly to `zombiewood.json`: graphics backend (D3D11 / OpenGL; requires a restart), internal resolution, filtering,
VSync, FPS limit, fullscreen, volumes, skip videos, language (en, fr, de, es, it, pt, ja, ko, zh, ru; on restart),
gamepad dead zone, backups, and the **zombie cursor** (on/off and size 32/48/64/96 px; keys `cursor` and
`cursor_size`). The full reference of keys, environment variables and command-line options is in
[`docs/BUILD.md`](docs/BUILD.md).

Saves are stored in `saves/`, and the port makes automatic copies in `saves_backup/` (at startup and every time the game
overwrites a save).

## Troubleshooting

| Symptom | What to do |
|---|---|
| Black window / graphics glitch | F1 → Gráficos (Graphics) → Backend = `gl` and restart (or `"backend": "gl"` in `zombiewood.json`). |
| Missing DLLs (`VCRUNTIME`, `MSVCP140`) | They ship in the zip; make sure you extracted every file. Otherwise install the *Microsoft Visual C++ Redistributable 2015-2022 x64*. |
| No videos / logo | Windows N: install the *Media Feature Pack*; or set `"skip_videos": true`. |
| The installer warns about a different SHA-256 | Your APK/OBB is not the original 1.5.3; it may misbehave. |
| Stuttering | Disable VSync / try another backend; `fps_limit` 0 = unlimited. Check `logs/zombiewood.log`. |
| Choppy or crackling audio | F1 → Audio → *Latencia de audio* (audio latency) = *Segura* (safe) and restart (`"audio_latency": "safe"`). |
| The gamepad does not respond | Open F1 → Controles → *Mando*: if the tester does not react, close Steam/DS4Windows or set the driver to *Compatibilidad* and restart; then use the mapping wizard. |
| "Sending TapJoy request… / Response not valid" messages in the log | Normal without a network; they are summarized automatically. |

If something fails, attach `logs/zombiewood.log`. `ZW_TRACE_INPUT=1` traces input.

## Testing it on your PC and reporting results

So far it has only been tested on one PC (Windows 11, NVIDIA RTX 3060). If you try it on another one, open an *issue*
in the repository with:

1. Windows (version/edition), CPU and GPU (`dxdiag` → Display tab).
2. How far you got: wizard / APK installation / logo / intro / menu / gameplay, and the FPS (F1 → top bar).
3. Can you hear the audio? Do the videos play? Did you try a gamepad (which one)?
4. If it fails or closes: attach `logs\zombiewood.log` (next to the exe) and, if possible, a screenshot (F12 → `screens\` folder).

Quick fix if it does not start: in `zombiewood.json` change `"backend": "d3d11"` to `"gl"` and open it again.

## Building from source

See [`docs/BUILD.md`](docs/BUILD.md) (VS 2022 Build Tools + vcpkg + `win-release` preset). To build the package:

```powershell
pwsh tools\package.ps1 -Clean      # configures, builds, tests and creates dist\Zombiewood-Win64-<version>.zip
```

Technical documentation: [`docs/PLAN.md`](docs/PLAN.md), [`docs/ANALYSIS.md`](docs/ANALYSIS.md) (APK analysis),
[`docs/CONTRACTS.md`](docs/CONTRACTS.md) (interfaces between modules), [`docs/QA.md`](docs/QA.md).

### Application icon (optional)

The icon is not versioned (it is game art). To embed it in `zombiewood.exe`, generate the `.ico` from your image before building:

```powershell
pwsh tools/make_icon.ps1 -Source C:\path\to\your\icon.jpg   # writes resources/zombiewood.ico
```

CMake detects it and uses it as the exe icon (desktop, Explorer, taskbar and window).

### Game cursor (optional)

Like the icon, the cursor is not versioned. To embed it, generate the resource from a PNG with transparency (it uses the
figure in the left half of the image; `-Whole` uses the whole image):

```powershell
pwsh tools/make_cursor.ps1 -Source C:\path\to\cursor.png   # writes resources/zombiewood_cursor.bin
```

Without it, the game uses the normal Windows cursor.

## Changelog

Every version and improvement is listed in [`CHANGELOG.md`](CHANGELOG.md). Downloads are in the repository's *Releases* section.

## Credits and third-party licenses

This repository's own code is the runtime (loader, HLE, frontend). It uses, unmodified except where noted:

| Component | Used for | License |
|---|---|---|
| [SDL2](https://libsdl.org) | window, input, gamepads, audio | zlib |
| [ANGLE](https://chromium.googlesource.com/angle/angle) (vcpkg) | OpenGL ES 2/3 on Direct3D 11 / OpenGL | 3-clause BSD |
| [dynarmic](https://github.com/azahar-emu/dynarmic) (Azahar fork, commit `a466015`; local *shadow space* patch in the exception handler) and its externals (mcl, xbyak, Zydis/Zycore, fmt, robin-map, biscuit, oaknut) | ARM32 → x64 JIT recompiler | 0BSD (dynarmic); MIT/BSD for the externals |
| [Dear ImGui](https://github.com/ocornut/imgui) v1.91.9 | F1 menu and wizard | MIT |
| [zlib](https://zlib.net) | APK decompression | zlib |
| [nlohmann/json](https://github.com/nlohmann/json) | configuration | MIT |
| [Boost](https://www.boost.org) (ICL, Variant; headers) | dynarmic dependency | Boost Software License |
| [SDL_GameControllerDB](https://github.com/mdqinc/SDL_GameControllerDB) | gamepad mappings (`gamecontrollerdb.txt`) | zlib |
| Windows Media Foundation | H.264/AAC videos | system component |

The full license texts ship in `licenses/` inside the zip. *Zombiewood* is the property of its respective owners
(Gameloft / TechJ5 Games); this project is not affiliated with them, does not redistribute their content and requires a
legitimate copy.

## Legal

- **Project license:** this repository's own code and documentation (runtime, tools and docs) are released under the
  [MIT license](LICENSE) — © 2026 ChrisDutary. You may use, modify and redistribute them as long as you keep the
  copyright notice; they are provided "as is", **without warranty of any kind**.
- **The game is not part of the project:** *Zombiewood*, its name, logos, graphics, sounds, videos, code and data are the
  property of Gameloft and their respective owners. This repository and its downloads **do not include** any game
  content; every user must provide their own legitimate copy. The MIT license does **not** apply to that content.
- **No affiliation:** this is an independent, non-profit project, not related to or endorsed by Gameloft. All
  trademarks belong to their owners.
- **Third-party art:** the icon and cursor the executable may embed belong to their authors and are not covered by the
  MIT license. The libraries used keep their own licenses (see the table above and `licenses/`).
- If you hold any rights and want something removed, open an *issue* in the repository and it will be handled.
