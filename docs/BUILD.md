# Building Zombiewood from scratch (Windows x64)

## Requirements

- Windows 10/11 x64.
- **Visual Studio 2022 Build Tools** with the "Desktop development with C++" workload (MSVC x64, CMake, Ninja).
- **git**.
- **vcpkg** cloned at `C:\Users\chdut\Tools\vcpkg` with `bootstrap-vcpkg.bat` already run
  (if yours lives elsewhere, change `VCPKG_ROOT` and `toolchainFile` in `CMakePresets.json`).
- Internet access on the first configure (vcpkg and FetchContent download sources).

Dependencies (all pinned):

| Dependency | Source | Pinned by |
|---|---|---|
| SDL2, ANGLE (`angle`, GLES2 on D3D11), zlib, nlohmann-json, boost-icl/variant | vcpkg (manifest mode) | `builtin-baseline` in `vcpkg.json` |
| Dear ImGui v1.91.9 (+ SDL2 / OpenGL3 backends in ES2 mode) | FetchContent | tag |
| dynarmic (azahar-emu, A32 frontend + x64 backend, externals included) | FetchContent | commit `a466015` |

Logging and config only use the standard library (`std::format`); there is no spdlog.

## Building

From a shell with the MSVC environment loaded (for example "x64 Native Tools Command Prompt for VS 2022",
or `call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"`
and add the `cmake`/`ninja` from `Common7\IDE\CommonExtensions\Microsoft\CMake` to `PATH`), at the repository root:

```bat
cmake --preset win-debug
cmake --build --preset win-debug
ctest --preset win-debug
```

`win-release` works the same way. The build directory lives **outside the repo**:
`C:\Users\chdut\ZombiewoodDev\build\<preset>` (the repo lives in OneDrive).

The first configure takes a while (vcpkg builds ANGLE and SDL2 from source; ~10–20 min). Later ones are fast.

## Running

A folder with `zombiewood.exe` + `SDL2.dll`, `libEGL.dll`, `libGLESv2.dll` and `z.dll` is all an end user needs; they
provide the original game themselves (it is never in the repo). `<home>` is `ZOMBIEWOOD_HOME` if set, otherwise the
exe's folder.

```bat
C:\Users\chdut\ZombiewoodDev\build\win-release\src\frontend\zombiewood.exe
```

### First run (installer, T9)

If there is no data (`<home>/data/lib/libZombieWood.so` + main OBB), a wizard appears in the window itself:
**Elegir APK… / Choose APK… (Enter)** opens the Windows file dialog. Either works:

- the large APK (`assets/archiveObbCache.zip.001…00N` with the OBBs inside), or
- the original APK + `main.1501…obb` / `patch.15325…obb` (looked up next to the APK and in `obb/`; if missing, another
  dialog asks for them).

It is streamed into `<home>/data/`: `lib/libZombieWood.so`, `lib/libgenerator.so`, `obb/main…obb`, `obb/patch…obb`,
`res/raw/*.mp4` (+ text files), `assets/colors.conf`, `assets/families.list` and `install.json` (source, SHA-256,
warnings). The SHA-256 of `libZombieWood.so` (1.5.3 = `2855ed9f…fa967c`) and of the OBBs is verified; if they do not
match, a warning is shown (another version may not work). With the 306 MB APK it takes ~1–3 s on an SSD. Then the full
`zombiewood.json` is written and **Jugar / Play (Enter)**.

Headless: `zombiewood --install=<apk> [--obb=<main.obb> --obb=<patch.obb>] [--install-only]`; `--reinstall` reopens
the wizard even if data exists.

### Folders in `<home>`

| Folder / file | Contents |
|---|---|
| `zombiewood.json` (+ `.bak`) | configuration (edited by the F1 menu) |
| `data/` | installed game data + the guest's `/sdcard/android/data/<pkg>/files` (what the game writes) |
| `saves/` | the guest's `/data/data/<pkg>/`: `player.dat`, `zomb.dat`, `level.dat`, `zw_prefs.json`… |
| `saves_backup/` | automatic copies: `<date>_inicio/` at startup and `<date>/` with the previous version of every save the game overwrites (`backups_keep` are kept); `shadow/` is internal |
| `logs/` | `zombiewood.log` + `zombiewood.1.log`…`.4.log` (rotated at startup and past 32 MiB). Identical repeated Info lines (e.g. the game's "Sending TapJoy request… / Response not valid" spam without a network) are summarized as `(xN en 30 s, suprimidas) …`; `ZW_LOG_NODEDUP=1` disables this |
| `screens/` | screenshots (F12) |
| `imgui.ini` | positions of the menu windows |
| `gamecontrollerdb.txt` | optional, extra SDL gamepad mappings |

### `zombiewood.json`

All keys are optional (the comments exist only on this page):

```json
{
  "game_dir": "data",                     // lib/, obb/, res/raw/, assets/ (relative to <home> or absolute)
  "lib": "...", "assets_dir": "...", "videos_dir": "...", "screens_dir": "...",  // override the derived paths
  "vfs": { "data_dir": "data", "saves_dir": "saves", "obb_dir": "..." },
  "backend": "d3d11",                     // ANGLE: "d3d11" | "gl" (desktop OpenGL) | "vulkan" | "d3d9"; on restart
  "width": 1280, "height": 720, "fullscreen": false,
  "internal_width": 1280, "internal_height": 720,   // resolution the game renders at (scaled with letterbox)
  "internal_follows_window": false,                 // true: internal resolution = window (GL2JNILib.resize on change)
  "linear_filter": true, "vsync": true, "fps_limit": 60,   // 0 = unlimited; the game uses real time
  "show_fps": false, "menu_bar": false,             // FPS counter; menu bar visible at startup
  "cursor": "zombie", "cursor_size": 48,            // (0.2.0) embedded zombie cursor | "system"; 32/48/64/96 px
  "pause_on_focus_loss": false,                     // minimizing always pauses (pause + nativeVoxSuspend)
  "volume": 1.0, "video_volume": 1.0, "mute": false,
  "audio_latency": "normal",                        // (0.2.2) "low" 80 ms | "normal" 150 ms | "safe" 250 ms; on restart
  "pad_driver": "auto",                             // (0.2.2) "auto" (SDL HIDAPI) | "compat" (no HIDAPI); on restart
  "skip_videos": false, "play_logo": true,          // res/raw videos (logo at startup, intro, moviecomplete)
  "language": 0,                                    // 0 en,1 fr,2 de,3 es,4 it,5 pt,6 ja,7 ko,8 zh,9 ru (on restart)
  "backups_keep": 20,                               // number of save backup sets
  "input": { }                                      // see below; the Controls menu writes it in full
}
```

### Menu (F1)

Top bar in the style of Ship of Harkinian (labels currently in Spanish): **Zombiewood** (pause/resume, screenshot,
quit), **Gráficos / Graphics** (backend, internal resolution, filtering, VSync, FPS limit, fullscreen, show FPS, skip
videos, language, cursor), **Audio** (game and video volume, mute), **Controles / Controls** (remapping: "Cambiar" /
Change, then press a key or gamepad button; "+" adds, "x" deletes; dead zone, mouse), **Carpetas / Folders** (open
saves, backups, logs, screenshots, data; back up now) and **Ayuda / Help**. Changes are saved instantly to
`zombiewood.json`; backend and language require a restart. While the menu owns the mouse or keyboard, those events do
not reach the game.

### Videos

`Game.playCinematicAndroid` (intro and "movie complete") and the Gameloft logo at startup are played with Windows Media
Foundation (the system's H.264/AAC; Windows N needs the Media Feature Pack, otherwise they are skipped). Any key, click
or gamepad button skips them. `"skip_videos": true` always skips them.

### Controls

Default controls (the game only understands touches and BACK/MENU: keyboard and gamepad are translated into virtual
touches on the HUD, `src/frontend/input.cpp`):

| Action | Mouse / keyboard | Gamepad |
|---|---|---|
| Tap (menus, buttons) | left click (pointer 0); touch screens: multitouch | — |
| Move | `W A S D` (left virtual stick) | left stick |
| Aim and shoot | arrow keys, or hold right click (shoots toward the cursor) | right stick |
| Medkit / weapon / pause / accept (`>>`) | `E` · `Q` · `P` · `Space` | `Y` · `RB`/`X` · `Start` · `A` |
| Back (BACK) / MENU | `Esc` · `Tab` | `Back`/`B` · `Guide` |
| Port menu / PNG screenshot / fullscreen | `F1` · `F12` · `F11` or `Alt+Enter` | — |

The `"input"` object (merged over the defaults; `x`,`y` normalized 0–1, `radius` as a fraction of the height; keys =
`SDL_GetScancodeFromName` names, gamepad = `SDL_GameController` names):

```json
"input": {
  "deadzone": 0.25, "mouse_touch": true, "mouse_aim_button": "right", "player_pos": [0.5, 0.45],
  "move_stick": { "x": 0.133, "y": 0.82, "radius": 0.105, "keys": {"up":"W","down":"S","left":"A","right":"D"}, "pad_stick": "left" },
  "aim_stick":  { "x": 0.866, "y": 0.82, "radius": 0.105, "keys": {"up":"Up","down":"Down","left":"Left","right":"Right"}, "pad_stick": "right" },
  "buttons": [ { "name": "botiquin", "x": 0.934, "y": 0.542, "keys": ["E"], "pad": ["y"] } ],
  "android_keys": [ { "code": 4, "keys": ["Escape"], "pad": ["back"] } ]
}
```

### Command line and testing

`--lib=<so>`, `--no-audio`, `--uncapped` (no vsync or limit, for measuring; not saved to the json), `--frames=N`,
`--shot-every=N`, `--hidden` (hidden window; also skips the logo), `--install=<apk>`, `--obb=<file>`,
`--install-only`, `--reinstall`, `--script=<file>` / `--control=<file>` (test commands: `wait`, `tap`,
`down/move/up`, `key`, `sdlkey`, `pad`, `axis`, `mouse`, `shot`, `pause`, `resume`, `resize`, `quit` and, from T9,
`menu [graphics|audio|controls|about|none]`, `wshot <name>` (screenshot of the window, including the menu),
`remap <target> [add]` + `ui key <key>` / `ui pad <button>` (a real SDL event sent to the menu); with `--control` the
lines appended to the file are executed as they arrive), `--load-only <so>` (load diagnostics with the real HLE) and
`--smoke-test[=N]` (T2's window + ImGui test, no game). Environment: `ZW_TRACE_INPUT=1`, `ZW_AUDIO_DUMP=<pcm>`,
`ZW_LOG_NODEDUP=1`, `ANGLE_DEFAULT_PLATFORM` (overrides `backend`) and those in CONTRACTS §8.

- The post-build step copies `libEGL.dll`, `libGLESv2.dll` (ANGLE) and `z.dll` next to the exe. The context is
  requested as ES 3.0 (the game uses ES 2.0; the frontend needs `glBlitFramebuffer` to scale the internal framebuffer).

## Packaging the release (T10)

```powershell
pwsh tools\package.ps1 -Clean        # configures (win-release preset), builds, runs ctest, stages and zips
pwsh tools\package.ps1 -SkipBuild    # only repackages what is already in the build directory
```

Output (by default in `C:\Users\chdut\ZombiewoodDev\dist\`): a `Zombiewood-Win64\` folder with `zombiewood.exe`,
`SDL2.dll`, `libEGL.dll`, `libGLESv2.dll`, `z.dll`, the Visual C++ runtime (`msvcp140.dll`, `vcruntime140.dll`,
`vcruntime140_1.dll`), `gamecontrollerdb.txt` (Windows entries only, `tools/package/`), `readme.txt`
(`tools/package/readme.txt`), `README.md`, `LICENSE.txt` and `licenses/`; plus `Zombiewood-Win64-<version>.zip`
(+ `.sha256`). The script fails if the package contains `*.apk/.obb/.so/.mp4/.bar/.dex`, and it refuses to wipe the
staging folder if it contains `data/`, `saves/` or `zombiewood.json`. Parameters: `-DistDir`, `-BuildDir`, `-Clean`,
`-SkipBuild`, `-SkipTests`. `-Clean` does a true from-scratch build (vcpkg + FetchContent clones: ~15-20 min, needs a
network).

QA with real Windows input lives in `tools/qa/` (`qa_phase1…5`) and its results in [`QA.md`](QA.md).
When opened with a double click, the exe detaches its console (no black window); from a terminal it keeps it.

## Target layout

`zw_core` (log/config/paths), `zw_installer` (T9: APK → data, zlib + BCrypt), `zw_loader`, `zw_cpu` (+ `zw_cpu_deps` =
dynarmic), `zw_hle_libc`, `zw_hle_gles`, `zw_hle_audio`, `zw_jni`, `zw_frontend` (SDL2 window + GLES context, startup,
GL thread, input; T9: wizard, ImGui menu, Media Foundation videos, save backups; 0.2.0: cursor) and the `zombiewood`
executable. `ctest`: 8 tests, including `installer` (synthetic APKs, does not need the game data). zlib comes from
vcpkg (declared in `vcpkg.json`, `find_package(ZLIB)`).

Optional embedded resources (not versioned): `resources/zombiewood.ico` (`tools/make_icon.ps1`) and
`resources/zombiewood_cursor.bin` (`tools/make_cursor.ps1`); CMake adds the matching `.rc` only if the file exists.

## Notes

- **Debug outside vcvars**: the `win-debug` exe uses the debug CRT (`msvcp140d.dll`, ...). Run it from a shell with
  vcvars loaded, or add `...\BuildTools\VC\Redist\MSVC\<ver>\debug_nonredist\x64\Microsoft.VC143.DebugCRT` to `PATH`.
  `win-release` does not have this requirement.
- The post-build step also copies `z.dll` (vcpkg's ANGLE imports it even in debug).
- **ANGLE and ETC1** (measured on an RTX 3060 / D3D11): ANGLE 2.1.7258 exposes an `OpenGL ES 3.0` context,
  `GL_OES_compressed_ETC2_*`, `GL_EXT_texture_compression_dxt1`, `GL_ANGLE_texture_compression_dxt5`, but **NOT**
  `GL_OES_compressed_ETC1_RGB8_texture` or `GL_OES_depth_texture`. ETC2 RGB8 decodes ETC1 blocks identically, so T5 can
  map `GL_ETC1_RGB8_OES` to `GL_COMPRESSED_RGB8_ETC2` (or decompress on the CPU).
