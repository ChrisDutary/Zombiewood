# T10 — Win64 packaging, README and QA

> Historical task spec (November 2026). Paths refer to the original development machine.

Repo on `main`. Read all of `docs/`.

## Goal
A reproducible release and user documentation.

## Requirements
- `win-release` preset → a `Zombiewood-Win64/` folder (in `C:\Users\chdut\ZombiewoodDev\dist\`) with `zombiewood.exe`, the ANGLE/SDL DLLs, `gamecontrollerdb.txt`, `readme.txt`; a final zip. No game data.
- `Zombiewood/README.md` (in Spanish at the time): what it is, that it does **not** include the game (every user provides their own copy), requirements, installation, default controls, configuration, troubleshooting, third-party credits/licenses (SDL2, ANGLE, dynarmic, ImGui…).
- A `tools/package.ps1` script that builds the zip from scratch.
- QA: a checklist (clean start, data install, menu, 10-minute match, save/load, gamepad, windowed/fullscreen switching, alt-tab, audio) with results and any bugs found, in `docs/QA.md`.

## Proof
The generated zip (path and size), a complete QA.md, the README.

## Constraints
Commits `zombiewood(T10): ...`, no push.
