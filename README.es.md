# Zombiewood para Windows (port nativo)

[English](README.md) · **Español**

Runtime para jugar **Zombiewood 1.5.3** (juego de Android, de Gameloft) de forma nativa en Windows 10/11 x64, este proyecto es **solo el entorno de ejecución**. Carga la biblioteca nativa del propio
juego (`libZombieWood.so`, ARM de 32 bits) con un cargador ELF propio, la ejecuta con un recompilador JIT (dynarmic) y
sustituye Android por Windows: libc/pthread de bionic, OpenGL ES 2 (ANGLE sobre Direct3D 11), audio (SDL2), JNI, vídeo
(Media Foundation) y entrada (teclado, ratón, mando → toques virtuales sobre el HUD).

> ## ⚠️ No incluye el juego
> Este repositorio y el paquete de descarga **no contienen ningún dato de Zombiewood** (ni código, ni gráficos, ni sonido,
> ni vídeos). Cada usuario debe aportar **su propia copia legítima** del juego; el instalador integrado extrae lo necesario
> de tu APK. Los archivos del juego (`*.apk`, `*.obb`, `*.so`, `data/`) están excluidos en `.gitignore` y el script de
> empaquetado se niega a generar un zip que los contenga.

Estado: jugable a 60 fps (tutorial, menú de películas y la escena 1 "Body Count" completa, con guardado y carga; ver [`docs/QA.md`](docs/QA.md) para lo
probado y lo no probado: mando físico y escenas 2+ sin probar). Sin conexión: los servidores de Gameloft ya no existen, así que tienda online, TapJoy, etc. no funcionan.

## Requisitos

- Windows 10 u 11 de 64 bits.
- GPU con Direct3D 11 (el backend por defecto); alternativamente OpenGL de escritorio.
- Para los vídeos: Windows Media Foundation (incluido en Windows; en ediciones **N/KN** instala el *Media Feature Pack*, si
  no, los vídeos se saltan).
- Tu copia del juego, versión **1.5.3** (paquete `techj5gameszombiew153`):
  - el APK grande (≈306 MB, trae los OBB dentro en `assets/archiveObbCache.zip.001…`), **o**
  - el APK original + `main.1501.….obb` y `patch.15325.….obb`.
  Otras versiones pueden no funcionar (el instalador avisa si los SHA-256 no coinciden).
- ~1 GB libre en disco para los datos instalados.

## Instalación

1. Descarga `Zombiewood-Win64-<versión>.zip` desde la sección **Releases** del repositorio
   ([última versión](https://github.com/ChrisDutary/Zombiewood/releases/latest)) y descomprímelo
   en una carpeta con permiso de escritura (p. ej. `C:\Juegos\Zombiewood`, **no** en `Archivos de programa` ni dentro del
   zip). Contiene `zombiewood.exe`, `SDL2.dll`, `libEGL.dll`, `libGLESv2.dll`, `z.dll`, el runtime de Visual C++
   (`msvcp140.dll`, `vcruntime140.dll`, `vcruntime140_1.dll`), `gamecontrollerdb.txt`, `readme.txt`, `LICENSE.txt` y `licenses/`.
   Opcional: comprueba el SHA-256 publicado junto al zip (`Get-FileHash .\Zombiewood-Win64-<versión>.zip`).
2. Ejecuta `zombiewood.exe`. El exe no está firmado: si Windows SmartScreen muestra *"Windows protegió tu PC"*, pulsa
   **Más información → Ejecutar de todas formas**. Como aún no hay datos, aparece el asistente de primer arranque dentro de la ventana.
3. **Elegir APK… (Enter)** → selecciona tu APK. Se extrae en streaming a `data/` (unos segundos en un SSD), se verifican los
   SHA-256 y se escribe `zombiewood.json`.
4. **Jugar (Enter)**. Verás el logo, la intro (cualquier tecla la salta) y el juego.

Todo lo que genera el port queda junto al exe (o en `ZOMBIEWOOD_HOME` si defines esa variable de entorno): `data/`, `saves/`,
`saves_backup/`, `logs/`, `screens/`, `zombiewood.json`. Para desinstalar, borra la carpeta.

Sin interfaz: `zombiewood --install=<apk> [--obb=<main.obb> --obb=<patch.obb>] [--install-only]`; `--reinstall` reabre el asistente.

### Actualizar sin perder tu partida

Cierra el juego y copia el **contenido** de la carpeta `Zombiewood-Win64` del zip nuevo encima de tu carpeta del juego
(*Reemplazar los archivos en el destino*). El zip solo trae el programa: `saves/`, `data/` y `zombiewood.json` no se tocan.

### Pasar tu partida a otra PC

Instala el port y tu APK en la PC nueva, ábrelo una vez, ciérralo y copia el contenido de tu carpeta `saves/` en la nueva.
Las partidas no están atadas a la máquina.

## Controles por defecto

El juego solo entiende toques y las teclas BACK/MENU de Android; el port traduce teclado y mando a toques virtuales.

| Acción | Teclado / ratón | Mando |
|---|---|---|
| Tocar (menús, botones) | clic izquierdo | — |
| Mover | `W A S D` | stick izquierdo |
| Apuntar y disparar | flechas, o mantener clic derecho | stick derecho |
| Botiquín / arma / pausa / aceptar | `E` · `Q` · `P` · `Espacio` | `Y` · `RB`/`X` · `Start` · `A` |
| Atrás / menú del juego | `Esc` · `Tab` | `Back`/`B` · `Guide` |
| Menú del port · captura · pantalla completa | `F1` · `F12` · `F11` / `Alt+Enter` | — |

Todo es remapeable desde **F1 → Controles** (botón *Cambiar* y pulsa una tecla o botón; *+* añade un segundo enlace).
`gamecontrollerdb.txt` incluye mapeos SDL de mandos de Windows; puedes añadir los tuyos.

**Mandos (0.2.2+):** la sección *Mando* de F1 → Controles muestra el mando conectado, un probador en vivo y los nombres de
botones de PlayStation. Usa **Asistente: mapear con el mando** para asignar cada acción pulsándola en el mando, e
*Intercambiar sticks* para cambiar cuál mueve y cuál apunta. Si el mando aparece pero no reacciona, cierra Steam/DS4Windows
(pueden quedarse con el control exclusivo de los mandos de PlayStation) o pon *Driver de mandos* en *Compatibilidad* y reinicia.

## Configuración

El menú **F1** (Zombiewood · Gráficos · Audio · Controles · Carpetas · Ayuda) guarda al instante en `zombiewood.json`:
backend gráfico (D3D11 / OpenGL; pide reiniciar), resolución interna, filtrado, VSync, límite de FPS, pantalla completa,
volúmenes, saltar vídeos, idioma (en, fr, de, es, it, pt, ja, ko, zh, ru; al reiniciar), zona muerta del mando, copias de
seguridad, y el **cursor zombi** (on/off y tamaño 32/48/64/96 px; claves `cursor` y `cursor_size`). La referencia completa de claves, variables de entorno y opciones de línea de comandos está en
[`docs/BUILD.md`](docs/BUILD.md).

Las partidas están en `saves/` y el port hace copias automáticas en `saves_backup/` (al arrancar y cada vez que el juego
sobrescribe un save).

## Problemas comunes

| Síntoma | Qué hacer |
|---|---|
| Ventana negra / fallo gráfico | F1 → Gráficos → Backend = `gl` y reinicia (o `"backend": "gl"` en `zombiewood.json`). |
| Faltan DLL (`VCRUNTIME`, `MSVCP140`) | Vienen en el zip: comprueba que extrajiste todos los archivos. Si no, instala el *Microsoft Visual C++ Redistributable 2015-2022 x64*. |
| Sin vídeos / logo | Windows N: instala el *Media Feature Pack*; o pon `"skip_videos": true`. |
| El instalador avisa de SHA-256 distinto | Tu APK/OBB no es la 1.5.3 original; puede funcionar mal. |
| Tirones | Quita VSync/prueba otro backend; `fps_limit` 0 = sin límite. Mira `logs/zombiewood.log`. |
| Audio entrecortado o con chasquidos | F1 → Audio → *Latencia de audio* = *Segura* y reinicia (`"audio_latency": "safe"`). |
| El mando no responde | Abre F1 → Controles → *Mando*: si el probador no reacciona, cierra Steam/DS4Windows o pon el driver en *Compatibilidad* y reinicia; luego usa el asistente de mapeo. |
| Mensajes "Sending TapJoy request… / Response not valid" en el log | Normal sin red; se resumen automáticamente. |

Si algo falla, adjunta `logs/zombiewood.log`. `ZW_TRACE_INPUT=1` traza la entrada.

## Probarlo en tu PC y reportar resultados

Hasta ahora solo se ha probado en una PC (Windows 11, NVIDIA RTX 3060). Si lo pruebas en otra, abre un *issue* en el
repositorio (o escribe al autor) con:

1. Windows (versión/edición), CPU y GPU (`dxdiag` → pestaña Pantalla).
2. Hasta dónde llegaste: asistente / instalación del APK / logo / intro / menú / partida, y los FPS (F1 → barra superior).
3. ¿Se oye el audio? ¿Funcionan los vídeos? ¿Probaste un mando (cuál)?
4. Si falla o se cierra: adjunta `logs\zombiewood.log` (junto al exe) y, si puedes, una captura (F12 → carpeta `screens\`).

Prueba rápida si no arranca: en `zombiewood.json` cambia `"backend": "d3d11"` por `"gl"` y vuelve a abrirlo.

## Compilar desde el código

Ver [`docs/BUILD.md`](docs/BUILD.md) (VS 2022 Build Tools + vcpkg + preset `win-release`). Para generar el paquete:

```powershell
pwsh tools\package.ps1 -Clean      # configura, compila, prueba y crea dist\Zombiewood-Win64-<versión>.zip
```

Documentación técnica: [`docs/PLAN.md`](docs/PLAN.md), [`docs/ANALYSIS.md`](docs/ANALYSIS.md) (análisis del APK),
[`docs/CONTRACTS.md`](docs/CONTRACTS.md) (interfaces entre módulos), [`docs/QA.md`](docs/QA.md).

### Icono de la aplicación (opcional)

El icono no se versiona (es arte del juego). Para incrustarlo en `zombiewood.exe`, genera el `.ico` desde tu imagen antes de compilar:

```powershell
pwsh tools/make_icon.ps1 -Source C:\ruta\a\tu\icono.jpg   # escribe resources/zombiewood.ico
```

CMake lo detecta y lo usa como icono del exe (escritorio, Explorador, barra de tareas y ventana).

### Cursor del juego (opcional)

Igual que el icono, el cursor no se versiona. Para incrustarlo, genera el recurso desde un PNG con transparencia (usa la
figura de la mitad izquierda de la imagen; `-Whole` para usar la imagen entera):

```powershell
pwsh tools/make_cursor.ps1 -Source C:\ruta\a\cursor.png   # escribe resources/zombiewood_cursor.bin
```

Sin él, el juego usa el cursor normal de Windows.

## Historial de cambios

Todas las versiones y mejoras están en [`CHANGELOG.md`](CHANGELOG.md). Descargas en la sección *Releases* del repositorio.

## Créditos y licencias de terceros

El código propio de este repositorio es el runtime (cargador, HLE, frontend). Usa, sin modificar salvo donde se indica:

| Componente | Uso | Licencia |
|---|---|---|
| [SDL2](https://libsdl.org) | ventana, entrada, mandos, audio | zlib |
| [ANGLE](https://chromium.googlesource.com/angle/angle) (vcpkg) | OpenGL ES 2/3 sobre Direct3D 11 / OpenGL | BSD de 3 cláusulas |
| [dynarmic](https://github.com/azahar-emu/dynarmic) (fork de Azahar, commit `a466015`; parche local de *shadow space* en el manejador de excepciones) y sus externals (mcl, xbyak, Zydis/Zycore, fmt, robin-map, biscuit, oaknut) | recompilador JIT ARM32 → x64 | 0BSD (dynarmic); MIT/BSD los externals |
| [Dear ImGui](https://github.com/ocornut/imgui) v1.91.9 | menú F1 y asistente | MIT |
| [zlib](https://zlib.net) | descompresión del APK | zlib |
| [nlohmann/json](https://github.com/nlohmann/json) | configuración | MIT |
| [Boost](https://www.boost.org) (ICL, Variant; cabeceras) | requisito de dynarmic | Boost Software License |
| [SDL_GameControllerDB](https://github.com/mdqinc/SDL_GameControllerDB) | mapeos de mandos (`gamecontrollerdb.txt`) | zlib |
| Windows Media Foundation | vídeos H.264/AAC | componente del sistema |

Los textos de licencia completos viajan en `licenses/` dentro del zip. *Zombiewood* es propiedad de sus respectivos titulares
(Gameloft / TechJ5 Games); este proyecto no está afiliado a ellos, no redistribuye su contenido y requiere una copia legítima.

## Legal

- **Licencia del proyecto:** el código y la documentación propios de este repositorio (runtime, herramientas y docs) se
  publican bajo la [licencia MIT](LICENSE) — © 2026 ChrisDutary. Puedes usarlos, modificarlos y redistribuirlos
  manteniendo el aviso de copyright; se entregan "tal cual", **sin garantía de ningún tipo**.
- **El juego no es parte del proyecto:** *Zombiewood*, su nombre, logotipos, gráficos, sonidos, vídeos, código y datos son
  propiedad de Gameloft y de sus respectivos titulares. Este repositorio y sus descargas **no incluyen** ningún contenido
  del juego; cada usuario debe aportar su propia copia legítima. La licencia MIT **no** se aplica a ese contenido.
- **Sin afiliación:** este es un proyecto independiente y sin ánimo de lucro, sin relación con Gameloft ni respaldado por
  ella. Todas las marcas pertenecen a sus dueños.
- **Arte de terceros:** el icono y el cursor que puede incrustar el ejecutable pertenecen a sus autores y no están cubiertos
  por la licencia MIT. Las bibliotecas usadas mantienen sus propias licencias (ver la tabla anterior y `licenses/`).
- Si eres titular de algún derecho y quieres que se retire algo, abre un *issue* en el repositorio y se atenderá.
