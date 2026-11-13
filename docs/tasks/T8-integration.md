# T8 — Integration: startup, game loop, input; getting to play

> Historical task spec (November 2026). Paths refer to the original development machine.

Read `docs/PLAN.md`, `ANALYSIS.md`, `CONTRACTS.md`, `BUILD.md` and the code from T3–T7. Repo on `main`.

## Goal
`zombiewood.exe` starts Zombiewood with the data in `C:\Users\chdut\ZombiewoodDev\gamedata\` and it can be played: main menu → match → shoot/move → back to the menu.

## Requirements
- A startup sequence identical to the one T1 documents (UI/GL threads included), with `setPaths` pointing to T4's VFS.
- Loop: `step`/`nativeRender` once per frame on the GL thread, `SDL_GL_SwapWindow`, frame pacing at 30/60 as the game expects, `resize` when the window changes, pause/resume on minimize.
- Input: mouse → touch events (id 0, actions 0/1/2), optional multitouch; Esc → BACK, Tab → MENU (key-up, ANALYSIS §9).
  The game does **not** support gamepads (T1): keyboard and gamepad (SDL_GameController + `gamecontrollerdb`) are translated into **virtual touches**
  on the on-screen control areas (virtual move/aim joysticks, fire/reload/weapon buttons) using positions measured from HUD
  screenshots; neutral accelerometer. Map configurable in `zombiewood.json`.
- Resolution/orientation: landscape, render at a configurable internal resolution, scaling with aspect ratio.
- Debug and fix collaboratively: if the failure belongs to another module, make the minimal fix and explain it in the commit (you may touch `src/**` in this task), or `ask` if it is a contract change.
- Screenshots: save screenshots (`F12`) in `C:\Users\chdut\ZombiewoodDev\screens\` as evidence.

## Proof
Screenshots of the logo/main menu, of a match in progress and of the log without critical pending imports; a description of what works/does not work (audio, videos, saving), measured FPS.

## Constraints
Commits `zombiewood(T8): ...`, no push. No game data in the repo.
