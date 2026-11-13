# QA del release Win64 (T10)

Fecha: 2026-11-13. Máquina: Windows 11 Home 10.0.26200, pantalla 1920×1080, NVIDIA GeForce RTX 3060 Laptop (ANGLE D3D11,
GL ES 3.0). Artefacto probado: **el zip generado por `tools/package.ps1`** descomprimido en una carpeta vacía
(`ZombiewoodDev\t10qa`), nunca el árbol de build. Datos del juego: APK 1.5.3 completo (306 MB) del propietario.

Cómo se probó: **entrada real de Windows** (`keybd_event` con scancode, `mouse_event`, `SetWindowPos`, `ShowWindow`, diálogo
de archivos real de Windows) automatizada con los scripts de `tools/qa/` (`qa_phase1…5`); capturas de la ventana en
`ZombiewoodDev\t10qa\qa_shots\`, logs en `t10qa\logs\` y salidas de cada fase en `ZombiewoodDev\t10qa_phase*.txt`.
Se marca **NO PROBADO** todo lo que no pudo ejercitarse de verdad (no hay nadie que pulse un mando ni oídos para el audio).

## Lista de verificación

| # | Prueba | Resultado | Evidencia / notas |
|---|---|---|---|
| 1 | **Arranque limpio** (solo el contenido del zip, sin `data/`) | ✅ OK | `1_bienvenida.png`: asistente "No se encontraron los datos del juego". Se crean junto al exe `logs/`, `zombiewood.json`, `imgui.ini`, `saves/`, `saves_backup/`. Sin consola negra (ver B1, corregido). |
| 2 | **Instalar datos** desde el APK (diálogo real de Windows + ruta pegada) | ✅ OK | 13 ficheros en **1.0–1.3 s**; SHA-256 de `libZombieWood.so` y de ambos OBB = 1.5.3, 0 avisos; `3_instalado.png`; `data/` = 313 MB. |
| 3 | **Logo + intro** (Media Foundation) | ✅ OK | `4_logo.png`, `5_intro.png`; cualquier tecla salta el vídeo. |
| 4 | **Menú F1**: abrir/cerrar con `F1`, clic real en *Gráficos*, *Audio*; clic real en *Mostrar FPS* | ✅ OK | `p4_menu_graficos.png`, `p4_03_menu_audio.png`; `zombiewood.json` pasa de `show_fps` false→true y vuelve a false con clic real. Mientras el menú tiene el ratón, el clic no llega al juego. El resto de entradas (Controles/remapeo, Carpetas, Ayuda) se probaron en T9 con eventos SDL/teclado, **no con clics reales** en esta ronda. |
| 5 | **Partida de 10 min** con W A S D + flechas + clic derecho + clic/Espacio, 204 ciclos de entrada | ✅ OK | 10.6 min sin cierres ni cuelgues; **119 de 121 muestras de fps = 60.0** (una de 59.1 y la del arranque), step medio 5.4–6.0 ms, máx 12.7 ms; 36 090 frames; hilos guest estables en 18. `p2_*_min.png`. |
| 6 | **Escena completa** ("Body Count", clics reales para elegir escena y START, 109 s) | ✅ OK | Resultados: +380 monedas (555→935), 3 %→37 % de nivel, rollos 0→4/520 (`p5_90_final.png`). |
| 7 | **Guardar / cargar** | ✅ OK | Tras cerrar la ventana con la escena superada y relanzar: 935 monedas, 37 %, 4/520 y el juego retoma en la tienda de armas (`p5_99_recarga.png`). Cerrar con la ventana llama a `pause` y el juego guarda; `player.dat` cambia (MD5) y `saves_backup/<fecha>/` conserva la versión anterior. Salir en mitad de una escena no guarda las monedas de esa escena (comportamiento del juego, `p4_08_recarga.png`: 555). |
| 8 | **Mando** | ⚠️ **NO PROBADO** | SDL detecta un "DualSense Wireless Controller" y carga 583 mapeos de `gamecontrollerdb.txt`, pero nadie lo manipuló: la ruta stick/botones→toques solo está probada con ejes y botones inyectados en el mapeador (T8) y con eventos SDL de teclado/mando sintéticos para el remapeo (T9). |
| 9 | **Fullscreen / ventana** | ✅ OK | `F11` y `Alt+Enter`: 1296×759 → 1920×1080 (escritorio sin bordes) y de vuelta a la posición previa. Redimensionar a 800×600, 1600×900, 640×360 y 1280×720: letterbox correcto, UI recolocada, 60 fps (`p3_04_resize_*.png`). |
| 10 | **Minimizar / restaurar** | ✅ OK | Minimizar → log `pausa: GL2JNILib.pause + nativeVoxSuspend`; restaurar → `reanudar: GL2JNILib.resume + nativeVoxResume`; la imagen se recupera a 60 fps (`p3_05`, `p3_07`). Probado en el menú de películas, no en plena escena. |
| 11 | **Alt-Tab** | ✅ OK | El primer plano cambia a otra ventana y vuelve; con la ventana en segundo plano el juego sigue a 60 fps (por defecto `pause_on_focus_loss` = false, a propósito); la entrada con ratón/teclado se verificó en las fases de juego. Probado en el menú de películas. |
| 12 | **Audio** | ⚠️ Parcial | Dispositivo SDL abierto, 172 KB/s (44.1 kHz estéreo 16 bit) constantes; volcado PCM de 10 min (`ZW_AUDIO_DUMP`, 109 MB): RMS 4500–5700 y picos 24–32 k en todo el recorrido, sin silencios ni saturación sostenida. **No verificado de oído** (sesión sin escuchar). Los vídeos tampoco se oyeron. |
| 13 | **Cierre ordenado** | ✅ OK | `CloseMainWindow` → código de salida 0 en todas las fases (también con el menú F1 abierto y en pausa). |
| 14 | **Empaquetado** | ✅ OK | Ver "Paquete" más abajo. El script se niega a empaquetar `*.apk/.obb/.so/.mp4/.bar/.dex`. |

## Medidas de arranque (win-release, medido en `zombiewood.log`)

- Carga del ELF (35 578 símbolos, 364 imports, **552 constructores**): `.init_array` **204–227 ms** (5 ejecuciones).
- Primer `step` del juego (SetupDevice + OBB): **3.9–4.0 s**; después 60 fps estables (los 2–3 primeros segundos tras el
  primer frame tienen un `step` máx de ~100 ms por carga de texturas).
- Del inicio de la carga al primer frame: ≈ **5.5 s** con el logo saltado; con el logo de 11 s completo ≈ 16 s.
- Build limpio (vcpkg + FetchContent desde cero, `win-release-T10`): **configure y compilación sin errores**, ctest **8/8**.
  No hubo cuelgues de clones de FetchContent (imgui y dynarmic con submódulos descargaron a la primera).

## Bugs y observaciones

| ID | Gravedad | Descripción | Estado |
|---|---|---|---|
| B1 | media | Al abrir `zombiewood.exe` con doble clic aparecía una **ventana de consola negra** junto al juego (el exe es de consola por la CLI `--install`, ctest y los logs). Visible en las capturas de las fases 1–3 (texto en el borde izquierdo) y en Alt-Tab. | **Corregido** en `src/frontend/main.cpp`: si `GetConsoleProcessList` indica que la consola es solo nuestra, `FreeConsole()`; desde un terminal se conserva. Verificado: 0 `conhost` hijos. |
| B2 | baja | `Esc` (BACK) en plena escena: la primera pulsación no mostró nada a los 2 s y la segunda abrió el menú PAUSE (`p4_07_pausa_esc.png` / `p4_07b_esc2.png`). Visto una vez; T8 había visto Esc → QUIT. Puede ser la animación del juego o la sincronización del toque virtual BACK. | **Abierto**, no investigado. |
| B3 | info | El guest imprime `error: can't find method gotoAndStop / broadcastEvent / preloadGlyphs` y `error Initing Gaia` en su stdout (~10 líneas, ya presentes en T8). Sin efecto visible. | Comportamiento del juego. |
| B4 | info | Sin red: `Sending TapJoy request…` (se resume en el log) y tienda online/IAP/GLLive/Facebook sin servidores. | Esperado. |
| B5 | info | Clic en la cabecera de un menú de la barra F1 cuando otro menú ya está abierto con el ratón encima cierra el menú (comportamiento estándar de ImGui: el *hover* ya lo había abierto). | Esperado. |
| B6 | info | Cada guardado del juego crea una carpeta en `saves_backup/` (con varios minimizar/restaurar y relanzar hubo 21); `backups_keep` (20) recorta las antiguas. | Esperado. |

## No probado / limitaciones conocidas

- Mando físico (pulsaciones reales), clics reales en *Controles/Carpetas/Ayuda* del menú y en el remapeo.
- Audio de oído; reproducción del vídeo `moviecomplete` en su punto real del juego (requiere completar escenas).
- Escenas 2–4 y todo lo posterior a la tienda; tienda/IAP; idiomas distintos del inglés; `fps_limit`/`vsync` fuera de sus valores por defecto.
- Backends ANGLE Vulkan y D3D9 (ofrecidos en el menú, solo se probaron D3D11 y OpenGL en T9).
- Minimizar, Alt-Tab y fullscreen **en plena partida** (se probaron en el menú de películas; el camino de pausa es el mismo).
- Otras GPUs (AMD/Intel), pantallas HiDPI/multi-monitor, Windows 10, Windows N sin Media Feature Pack, rutas con espacios o Unicode, SmartScreen/antivirus sobre el exe sin firmar.
- Rendimiento medido solo en una RTX 3060 Laptop.

## Paquete

Generado con `pwsh tools\package.ps1 -Clean` (configura con `cmake --preset win-release`, compila, ctest 8/8, monta y
comprime). Ruta y tamaño: ver el pie de este fichero y el reporte final de T10.
