# T10 — Empaquetado Win64, README y QA

Repo en `main`. Lee todo `docs/`.

## Objetivo
Release reproducible y documentación de usuario.

## Requisitos
- Preset `win-release` → carpeta `Zombiewood-Win64/` (en `C:\Users\chdut\ZombiewoodDev\dist\`) con `zombiewood.exe`, DLLs de ANGLE/SDL, `gamecontrollerdb.txt`, `readme.txt`; zip final. Sin datos del juego.
- `Zombiewood/README.md` (español): qué es, que **no** incluye el juego (cada usuario aporta su copia), requisitos, instalación, controles por defecto, configuración, problemas comunes, créditos/licencias de terceros (SDL2, ANGLE, dynarmic, ImGui…).
- Script `tools/package.ps1` que genera el zip desde cero.
- QA: lista de verificación (arranque limpio, instalar datos, menú, partida 10 min, guardar/cargar, mando, cambio de ventana/fullscreen, alt-tab, audio) con resultados y bugs encontrados en `docs/QA.md`.

## Evidencia
Zip generado (ruta y tamaño), QA.md completo, README.

## Restricciones
Commits `zombiewood(T10): ...`, sin push.
