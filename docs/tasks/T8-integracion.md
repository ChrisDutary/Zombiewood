# T8 — Integración: arranque, bucle de juego, input; llegar a jugar

Lee `docs/PLAN.md`, `ANALYSIS.md`, `CONTRACTS.md`, `BUILD.md` y el código de T3–T7. Repo en `main`.

## Objetivo
`zombiewood.exe` arranca Zombiewood con los datos de `C:\Users\chdut\ZombiewoodDev\gamedata\` y se puede jugar: menú principal → partida → disparar/moverse → volver al menú.

## Requisitos
- Secuencia de arranque idéntica a la que documenta T1 (hilos UI/GL incluidos), con `setPaths` apuntando al VFS de T4.
- Bucle: `step`/`nativeRender` por frame en el hilo GL, `SDL_GL_SwapWindow`, frame pacing a 30/60 según el juego, `resize` al cambiar ventana, pause/resume al minimizar.
- Input: ratón → eventos táctiles (id 0, acciones 0/1/2), multitouch opcional; Esc → BACK, Tab → MENU (key-up, ANALYSIS §9).
  El juego **no** soporta gamepad (T1): teclado y mando (SDL_GameController + `gamecontrollerdb`) se traducen a **toques virtuales**
  sobre las zonas de los controles en pantalla (joystick virtual de movimiento/apuntado, botones de disparo/recarga/arma) con
  posiciones medidas en las capturas del HUD; acelerómetro neutro. Mapa configurable en `zombiewood.json`.
- Resolución/orientación: landscape, render a resolución interna configurable, escalado con aspect ratio.
- Depurar y arreglar en coordinación: si el fallo es de otro módulo, haz el arreglo mínimo y explícalo en el commit (tienes permiso para tocar `src/**` en esta tarea), o `ask` si es un cambio de contrato.
- Capturas: guarda screenshots (`F12`) en `C:\Users\chdut\ZombiewoodDev\screens\` para la evidencia.

## Evidencia
Capturas del logo/menú principal, de una partida en curso y del log sin imports pendientes críticos; descripción de lo que funciona/no funciona (audio, vídeos, guardado), FPS medido.

## Restricciones
Commits `zombiewood(T8): ...`, sin push. Nada de datos del juego en el repo.
