# T3 — Loader ELF + CPU ARM32 (dynarmic) + thunks de imports + bridge + hilos

Lee `Zombiewood/docs/PLAN.md`, `docs/ANALYSIS.md` (T1) y `docs/BUILD.md` (T2). Repo en `main`.

## Objetivo
El núcleo del runtime: cargar `libZombieWood.so` (y `libgenerator.so`) en un espacio de memoria guest, ejecutarlo con
dynarmic, y permitir que cualquier import se implemente como función C++ del host. Define los **contratos** que usarán T4–T7.

## Requisitos
1. **Memoria guest**: espacio de 32 bits reservado en el host (fastmem: reserva 4 GiB, `guest_addr + base = host_ptr`), con
   allocator para mapear segmentos, heap guest (malloc de bionic lo implementará T4 encima de tu API), pilas por hilo, y zona de thunks.
   API: `g2h<T>(guest_addr)`, `h2g(ptr)`, `GuestMemory::map/alloc/free`, lectura/escritura de strings.
2. **Loader ELF32**: PT_LOAD, permisos, `.dynamic`, relocaciones que reporte T1 (al menos RELATIVE, ABS32, GLOB_DAT, JUMP_SLOT),
   símbolos exportados (tabla nombre→addr guest, usar `.hash`), `.init_array` y `JNI_OnLoad` invocables, `.ARM.exidx` disponible para
   `__gnu_Unwind_Find_exidx`. Soporta cargar varias libs con resolución cruzada.
3. **Imports → host**: cada símbolo importado se resuelve a un stub guest (`svc #n; bx lr` en ARM y equivalente Thumb si aplica) cuyo
   SVC despacha a un handler registrado. Registro tipo `ZW_HLE("glClear", hle_glClear)` con *marshalling* automático AAPCS
   **según el ABI que confirme T1 (softfp probablemente: floats/doubles en registros core, 64-bit alineado a par de registros,
   resto en pila)**. Soporta variádicas (printf-like reciben un `VaList` guest). Variables importadas (`__sF`, `_ctype_`,
   `__stack_chk_guard`, `_tolower_tab_`…) se resuelven a datos guest que el módulo HLE provee.
   Un import sin handler: log claro con nombre y LR la primera vez y devolver 0 (configurable a abortar).
4. **Bridge host→guest**: `call_guest(addr, args...)` reentrante (callbacks: qsort, pthread_create, bufferqueue de OpenSL, JNI_OnLoad) usando LR mágico de retorno.
5. **CPU/hilos**: un `Dynarmic::A32::Jit` por hilo guest en su propio hilo host; monitor exclusivo global compartido (LDREX/STREX);
   CP15 TPIDRURO/TLS por hilo (bionic TLS slots: TLS_SLOT_SELF, errno slot, etc. según T1); VFP/NEON habilitados; caché de código
   invalidable. API de hilos para T4 (`pthread_create` crea hilo host + Jit).
6. **Depuración**: traza opcional de llamadas a imports (`ZW_TRACE_IMPORTS=1`), volcado de registros + backtrace guest (usando símbolos del .so) ante fallos, y `addr2sym`.
7. **docs/CONTRACTS.md**: API pública exacta para T4–T7 (memoria, registro HLE, marshalling, call_guest, hilos, JNI hooks) con ejemplos. **Escríbelo pronto** (primer commit) y avisa al coordinador con `ask` cuando esté, porque T4–T7 arrancan con él; luego solo cambios compatibles.

## Entregables
`Zombiewood/src/loader/`, `src/cpu/`, `docs/CONTRACTS.md`, tests en `tests/cpu/` y `tests/loader/`.

## Pruebas (evidencia)
- Tests unitarios con pequeños blobs ARM/Thumb ensamblados a mano (o un `.so` de prueba generado con el clang de VS `--target=armv7a-linux-androideabi` si está disponible): llamada a import con args int/float/double/variádica, callback host→guest→host, dos hilos con LDREX/STREX.
- `zombiewood --load-only <ruta .so>`: carga `libZombieWood.so`, aplica relocs, lista imports sin handler, ejecuta `.init_array` y `JNI_OnLoad` con un `JavaVM*` mínimo (stub) sin crashear, o detalla dónde se detiene. Pega la salida.

## Restricciones
Edita solo `src/loader/`, `src/cpu/`, `tests/cpu/`, `tests/loader/`, `docs/CONTRACTS.md`, y el CMake de esas carpetas. Para cambiar CMake raíz o frontend, `ask`. Commits `zombiewood(T3): ...`, sin push.
