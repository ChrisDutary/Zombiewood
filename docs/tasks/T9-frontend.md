# T9 — Instalador de datos, configuración, saves y menú ImGui

Lee `docs/PLAN.md` y el estado actual del código. Repo en `main`.

## Objetivo
Experiencia tipo Ship of Harkinian para el usuario final.

## Requisitos
- Primer arranque sin datos: diálogo (SDL/Windows file dialog) para elegir **el APK del usuario** (el grande con `assets/archiveObbCache.zip.00N` o el pequeño + carpeta/archivos `.obb`).
  Extrae a `<home>/data/`: `lib/libZombieWood.so`, `lib/libgenerator.so`, OBB main+patch (o su contenido según cómo lo use el VFS), recursos que T1 indique. Verifica SHA-256 conocidos (`libZombieWood.so` = `2855ed9f95367d5dc592a2179c048383c6a177b7ca6da19a2919a2527cfa967c`) y avisa si la versión no es la 1.5.3 soportada.
- `zombiewood.json`: backend (ANGLE D3D11 / OpenGL vía ANGLE), resolución interna, vsync, fullscreen, volumen, mapa de controles, ruta de datos.
- Menú ImGui (tecla F1, barra superior como SoH): gráficos, audio, controles (remapeo), mostrar FPS, abrir carpeta de saves/logs.
- Saves en `<home>/saves/` (mapeados desde `/data/data/...`), backup automático antes de sobrescribir.
- Logs rotativos en `<home>/logs/`.

## Evidencia
Prueba desde carpeta limpia: exe + DLLs → elegir `C:\Users\chdut\OneDrive\Escritorio\techj5gameszombiew153.apk` → instala → arranca el juego. Capturas del menú.

## Restricciones
Edita `src/frontend/`, `src/installer/`, `src/core/` (config), tests y docs. Commits `zombiewood(T9): ...`, sin push.
