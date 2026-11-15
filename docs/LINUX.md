# Zombiewood on Linux (x86_64)

The Linux build is the same runtime as the Windows one (same game code path: ELF loader + dynarmic JIT + Android HLE).
Only the platform layer differs: the system's OpenGL ES / EGL (Mesa or the vendor driver) instead of ANGLE, POSIX
files and signals instead of Win32, and no video playback yet.

> The game is **not** included. You need your own Zombiewood 1.5.3 APK, exactly as on Windows.

## Requirements

- A 64-bit (x86_64) distribution with **glibc 2.38 or newer**: Ubuntu 24.04+, Debian 13+, Fedora 39+, current Arch /
  openSUSE Tumbleweed. (The release is built on Ubuntu 24.04; older distributions will refuse to start it with a
  `GLIBC_2.38 not found` error. Build from source there, see [`BUILD.md`](BUILD.md).)
- X11 or Wayland, and **OpenGL ES 3.0** through EGL (any Mesa driver from the last years, or the NVIDIA driver).
- PulseAudio, PipeWire (with its PulseAudio compatibility) or ALSA for sound.
- Optional: `zenity` or `kdialog` for the *Choose APK* file dialog. Without them the wizard shows a text field where
  you type or paste the path of the APK.
- ~1 GB of free disk space for the installed data.

SDL2 and its dependencies are bundled in both packages. The OpenGL ES / EGL libraries, glibc and the base X11/Wayland
libraries are always the system's.

## Downloads

| Package | For | Data folder |
|---|---|---|
| `Zombiewood-Linux-x86_64-<version>.AppImage` | most users: one file, no installation | `~/.local/share/zombiewood` |
| `Zombiewood-Linux-x86_64-<version>.tar.gz` | a portable folder, like the Windows zip | next to the `zombiewood` binary |

Each one has a `.sha256` file next to it: `sha256sum -c Zombiewood-Linux-x86_64-<version>.AppImage.sha256`.

## Using the AppImage

```sh
chmod +x Zombiewood-Linux-x86_64-<version>.AppImage
./Zombiewood-Linux-x86_64-<version>.AppImage
```

1. The first run shows the wizard: **Elegir APK… / Choose APK… (Enter)** and pick your APK (or type/paste its path if
   you have no `zenity`/`kdialog`). The data is extracted in a few seconds and its SHA-256 hashes are verified.
2. **Jugar / Play (Enter)**.

If your system has no FUSE 2 (`dlopen(): error loading libfuse.so.2`), either install it (`libfuse2` / `libfuse2t64` /
`fuse2`) or run the AppImage without mounting: `./Zombiewood-…AppImage --appimage-extract-and-run`.

Headless install and other options work as on Windows:

```sh
./Zombiewood-…AppImage --install=/path/to/techj5gameszombiew153.apk --install-only
./Zombiewood-…AppImage --reinstall        # reopen the wizard
```

## Using the tar.gz

```sh
tar xzf Zombiewood-Linux-x86_64-<version>.tar.gz
./Zombiewood-Linux-x86_64/zombiewood
```

The folder contains the `zombiewood` binary, `lib/` (bundled SDL2 and dependencies), `gamecontrollerdb.txt`,
`zombiewood.png`, `readme.txt`, `README.md`, `LINUX.md`, `LICENSE.txt` and `licenses/`. Extract it somewhere you can
write to: `data/`, `saves/`, `logs/`… are created next to the binary.

## Where the data lives (`<home>`)

`<home>` holds `data/` (installed game data), `saves/`, `saves_backup/`, `logs/`, `screens/`, `zombiewood.json` and
`imgui.ini` (see [`BUILD.md`](BUILD.md) for the meaning of each one). It is resolved in this order:

1. **`$ZOMBIEWOOD_HOME`**, if set.
2. Inside an AppImage (the binary lives in a read-only image), or whenever the binary's folder is not writable
   (e.g. installed under `/opt` or `/usr`): **`$XDG_DATA_HOME/zombiewood`**, which defaults to
   **`~/.local/share/zombiewood`**.
3. Otherwise the **folder of the `zombiewood` binary** (portable mode: the extracted tar.gz, or a build tree).

The first line of `logs/zombiewood.log` after the configuration (`Zombiewood - home: …`) tells you which one is in use.
To uninstall, delete the AppImage (or the extracted folder) and `<home>`.

To move a save between Windows and Linux, copy the contents of `saves/`: the format is the same.

The game's own files are looked up **case-insensitively** on Linux (the game mixes `android/Android`, `Sound/sound`…,
which only works by accident on Windows and Android's FAT SD cards), so you can also drop files into `data/` by hand.

## Known limits

- **No videos yet.** The Gameloft logo, the intro and the "movie complete" video are skipped (Windows plays them with
  Media Foundation). The log says so: `video: … saltado (sin reproductor en Linux)`. Gameplay is not affected.
- **`backend` is ignored.** The *Backend* option in F1 → Gráficos (and the `"backend"` key) selects an ANGLE backend
  and only applies to Windows. On Linux the renderer is whatever your EGL gives; check `GL_RENDERER` in the log.
- **Window icon on Wayland**: SDL2 cannot set it there; the desktop uses the icon of the AppImage's `.desktop` entry
  when the AppImage is integrated (AppImageLauncher, Gear Lever…). On X11 the icon is set by the game.
- **Steam Deck / SteamOS: untested.** SteamOS 3.6+ should satisfy the glibc requirement; older versions will not.
- **Tested only under WSL2 (WSLg)** on Ubuntu 24.04 with an NVIDIA RTX 3060 (Mesa D3D12 driver) and with llvmpipe. See the
  Linux section of [`QA.md`](QA.md).
  Native Mesa (Intel/AMD), the proprietary NVIDIA driver, physical gamepads and audio by ear have not been tested.
- The `--install-only` wizard keeps its error screen open if the APK is invalid (same as on Windows); close the window.

## Troubleshooting

| Symptom | What to do |
|---|---|
| `GLIBC_2.38 not found` | Your distribution is too old for the release binaries; build from source ([`BUILD.md`](BUILD.md)). |
| `dlopen(): error loading libfuse.so.2` | Install FUSE 2 or run with `--appimage-extract-and-run`. |
| Very low FPS, `GL_RENDERER : llvmpipe` in the log | Software rendering: install/enable your GPU's Mesa or vendor driver. Under **WSL2** run with `GALLIUM_DRIVER=d3d12` (WSLg picks llvmpipe by default) and keep SDL's default X11 backend: the `Failed to attach to x11 shm` messages on stderr are harmless, while WSLg's Wayland backend was seen presenting at only ~20 fps. |
| No window / `Could not initialize EGL` | Check that `libEGL.so.1` and `libGLESv2.so.2` exist (`libegl1`, `libgles2` on Debian/Ubuntu; `mesa-libEGL`, `mesa-libGLES` on Fedora). Try the other display server: `SDL_VIDEODRIVER=x11` or `wayland`. |
| No sound | `SDL_AUDIODRIVER=pulseaudio` (or `pipewire`, `alsa`); check that another app can play audio. |
| The file dialog does not open | Install `zenity` or `kdialog`, paste the path in the text field, or use `--install=<apk>`. |
| The gamepad does not respond | F1 → Controles → *Mando*: check the tester. Make sure your user can read `/dev/input/event*` / `hidraw*` (Steam's udev rules, or the `input` group); try *Driver de mandos* = *Compatibilidad*. |
| It closes unexpectedly | Attach `logs/zombiewood.log` and `logs/crash-<date>.txt` (signal, faulting module, backtrace and memory map) to your issue. |

Environment variables: `ZOMBIEWOOD_HOME`, the SDL ones (`SDL_VIDEODRIVER`, `SDL_AUDIODRIVER`,
`SDL_GAMECONTROLLERCONFIG_FILE`), Mesa's (`GALLIUM_DRIVER`, `LIBGL_ALWAYS_SOFTWARE`) and the port's own
(`ZW_TRACE_INPUT=1`, `ZW_LOG_NODEDUP=1`, `ZW_FASTMEM=0`, …; see [`BUILD.md`](BUILD.md) and CONTRACTS §8).

## How the Linux port differs internally

| Area | Windows | Linux |
|---|---|---|
| OpenGL ES | ANGLE (D3D11) shipped next to the exe | system EGL/GLES via SDL (`SDL_GL_CONTEXT_PROFILE_ES`), not bundled |
| Guest memory (4 GiB) | `VirtualAlloc` reserve/commit | `mmap(PROT_NONE, MAP_NORESERVE)` + `mprotect` / `madvise(MADV_DONTNEED)` |
| Fastmem faults | dynarmic's vectored exception handler (+ local shadow-space patch) | dynarmic's `SIGSEGV`/`SIGBUS` handler, unpatched |
| Host crash logging | `SetUnhandledExceptionFilter` + minidump `crash-*.dmp` | signal handlers chained under dynarmic's + `crash-*.txt` with a backtrace |
| Host files / VFS | Win32 handles, case-insensitive file system | `pread`/`pwrite`, per-component case-insensitive lookup |
| Installer hashes | portable SHA-256 (was BCrypt) | same code |
| File dialog | `GetOpenFileNameW` | `zenity` / `kdialog`, or a text field |
| Videos | Media Foundation | skipped |
| Icon and cursor | `.rc` resources | data generated at build time by `tools/embed/zw_embed.cpp` from the same optional files |
| `<home>` | folder of the exe | folder of the binary, or XDG data dir (AppImage / read-only folder) |

Platform code is isolated in `src/core/platform.*`, `src/core/paths.cpp`, `src/frontend/platform_{win,linux}.cpp`,
`src/frontend/video_linux.cpp` and `#ifdef _WIN32` blocks in `src/cpu/{guest_memory,cpu}.cpp` and
`src/hle/libc/{vfs,zip}.cpp`.
