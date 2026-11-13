# Changelog — Zombiewood para Windows

Todas las mejoras y cambios del port se documentan aquí. Formato basado en
[Keep a Changelog](https://keepachangelog.com/es-ES/1.1.0/); versiones con [SemVer](https://semver.org/lang/es/).
Cada versión se publica como release en [GitHub](https://github.com/ChrisDutary/Zombiewood/releases) con el tag `vX.Y.Z` y su zip
`Zombiewood-Win64-X.Y.Z.zip`.

**Actualizar sin perder tu partida:** cierra el juego y copia el contenido del zip nuevo encima de tu carpeta
(reemplazar archivos). El zip solo trae el programa; `saves/`, `data/` y `zombiewood.json` no se tocan.

## [Sin publicar]

Cambios ya en `main` que saldrán en la próxima versión.

### Añadido
- **Licencia MIT** (`LICENSE`, © 2026 ChrisDutary) para el código y la documentación propios del proyecto. El zip la
  incluirá como `LICENSE.txt`.
- Sección **Legal** en el README: el juego y su contenido son de Gameloft y no están cubiertos por la licencia; proyecto
  sin afiliación; arte de terceros (icono/cursor) fuera de la licencia; vía de contacto para retiradas.

## Repositorio

- 2026-11-13: el proyecto se mudó a su propio repositorio público [ChrisDutary/Zombiewood](https://github.com/ChrisDutary/Zombiewood)
  con todo su historial (antes vivía en la carpeta `Zombiewood/` de un repositorio de la universidad). Las releases
  0.1.0 y 0.2.0 se volvieron a publicar aquí con los mismos zips; los tags pasan a llamarse `v0.1.0` y `v0.2.0`.

## [0.2.0] — 2026-11-13

### Añadido
- **Cursor zombi** (flecha con ojo) en lugar de la flecha de Windows, en el juego, el asistente y el menú F1.
  Es el puntero del sistema (no lo dibuja el juego): no afecta a FPS ni a la lógica del juego.
- Opciones en **F1 → Gráficos**: *Cursor zombi* (on/off) y *Tamaño del cursor* (32 / 48 / 64 / 96 px).
  Claves nuevas en `zombiewood.json`: `"cursor": "zombie" | "system"` y `"cursor_size": 48`.
- `tools/make_cursor.ps1`: genera el cursor (`resources/zombiewood_cursor.bin`, incrustado en el exe como recurso)
  desde una imagen PNG con transparencia; recorta la figura y calcula la punta como punto de clic.
- Este `CHANGELOG.md`.

### Cambiado
- El paquete incluye el runtime de Visual C++ (`msvcp140.dll`, `vcruntime140.dll`, `vcruntime140_1.dll`) para que el
  juego abra en PCs sin el *VC++ Redistributable* (ya venía en el zip de 0.1.0 publicado; ahora queda documentado).
- `tools/package.ps1` se niega a regenerar la carpeta de staging si contiene `data/`, `saves/` o `zombiewood.json`
  (evita borrar una partida si alguien jugó desde `dist\Zombiewood-Win64`).
- Guía de instalación del README: descarga desde Releases, aviso de SmartScreen y sección para reportar pruebas en otras PCs.

### Notas
- El cursor y el icono son arte de terceros y no se versionan; se incrustan en el exe del release.
- Sin cambios en el núcleo (CPU/loader/HLE): las partidas de 0.1.0 siguen funcionando igual.

## [0.1.0] — 2026-11-13

Primera versión jugable.

### Añadido
- Runtime completo: cargador ELF ARM + JIT dynarmic + HLE de Android (libc/pthread de bionic, OpenGL ES → ANGLE/D3D11,
  audio AudioTrack → SDL2, JNI y clases Java emuladas).
- Instalador de primer arranque desde el APK del usuario (verifica SHA-256 de la versión 1.5.3).
- Menú F1 estilo Ship of Harkinian (gráficos, audio, controles con remapeo, carpetas, ayuda).
- Vídeos del juego con Media Foundation, copias de seguridad de saves, logs rotativos.
- Controles: ratón como toques, WASD/flechas y mando como toques virtuales sobre el HUD.
- Icono de la aplicación.

### Conocido
- En partida, Esc a veces necesita dos pulsaciones.
- Sin probar: mando físico, escenas 2+, otras GPUs. Servicios online (tienda, TapJoy, Facebook) no disponibles.
