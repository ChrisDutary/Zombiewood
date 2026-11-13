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
- Logs rotativos en `<home>/logs/`. Limita el spam del juego ("Sending TapJoy request… Response not valid", ~10 líneas/s
  en menús): agrupa repeticiones idénticas o cortocircuita la petición TapJoy en la capa JNI/red para que no se genere.
- **Vídeos** (pendiente de T8, hoy se saltan): los `.mp4` de `res/raw` del APK (`logo_hd`, `intro_hd`, `moviecomplete_hd/ld`…)
  que pide `SUtils` (ver ANALYSIS §8 y la clase emulada en `src/jni`). Extráelos en la instalación y reprodúcelos a pantalla
  completa con **Windows Media Foundation** (sin dependencias nuevas; `IMFSourceReader` → textura GLES vía ANGLE, audio por
  SDL) con salto por clic/tecla, y al terminar llama `SUtils.onVideoFinished` como hace Java. Si un vídeo falla, se salta como hoy.
  Opción `skip_videos` en `zombiewood.json`.

## Evidencia
Prueba desde carpeta limpia: exe + DLLs → elegir `C:\Users\chdut\OneDrive\Escritorio\techj5gameszombiew153.apk` → instala → arranca el juego. Capturas del menú.

## Restricciones
Edita `src/frontend/`, `src/installer/`, `src/core/` (config), tests y docs. Commits `zombiewood(T9): ...`, sin push.
