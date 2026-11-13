# T3 — ELF loader + ARM32 CPU (dynarmic) + import thunks + bridge + threads

> Historical task spec (November 2026). Paths refer to the original development machine; the project then lived in the
> `Zombiewood/` folder of another repository.

Read `Zombiewood/docs/PLAN.md`, `docs/ANALYSIS.md` (T1) and `docs/BUILD.md` (T2). Repo on `main`.

## Goal
The core of the runtime: load `libZombieWood.so` (and `libgenerator.so`) into a guest memory space, run it with
dynarmic, and let any import be implemented as a host C++ function. Define the **contracts** T4–T7 will use.

## Requirements
1. **Guest memory**: a 32-bit space reserved on the host (fastmem: reserve 4 GiB, `guest_addr + base = host_ptr`), with
   an allocator to map segments, a guest heap (T4 will implement bionic malloc on top of your API), per-thread stacks, and a thunk area.
   API: `g2h<T>(guest_addr)`, `h2g(ptr)`, `GuestMemory::map/alloc/free`, string read/write.
2. **ELF32 loader**: PT_LOAD, permissions, `.dynamic`, the relocations T1 reports (at least RELATIVE, ABS32, GLOB_DAT, JUMP_SLOT),
   exported symbols (name→guest addr table, using `.hash`), callable `.init_array` and `JNI_OnLoad`, `.ARM.exidx` available for
   `__gnu_Unwind_Find_exidx`. Support loading several libs with cross-resolution.
3. **Imports → host**: each imported symbol resolves to a guest stub (`svc #n; bx lr` in ARM and the Thumb equivalent if needed) whose
   SVC dispatches to a registered handler. Registration like `ZW_HLE("glClear", hle_glClear)` with automatic AAPCS *marshalling*
   **following the ABI T1 confirms (probably softfp: floats/doubles in core registers, 64-bit values aligned to a register pair,
   the rest on the stack)**. Support variadics (printf-like handlers receive a guest `VaList`). Imported variables (`__sF`, `_ctype_`,
   `__stack_chk_guard`, `_tolower_tab_`…) resolve to guest data provided by the HLE module.
   An import without a handler: a clear log with its name and LR the first time, and return 0 (configurable to abort).
4. **Host→guest bridge**: reentrant `call_guest(addr, args...)` (callbacks: qsort, pthread_create, OpenSL bufferqueue, JNI_OnLoad) using a magic return LR.
5. **CPU/threads**: one `Dynarmic::A32::Jit` per guest thread on its own host thread; a shared global exclusive monitor (LDREX/STREX);
   per-thread CP15 TPIDRURO/TLS (bionic TLS slots: TLS_SLOT_SELF, errno slot, etc. as per T1); VFP/NEON enabled; an invalidatable
   code cache. A thread API for T4 (`pthread_create` creates a host thread + Jit).
6. **Debugging**: optional tracing of import calls (`ZW_TRACE_IMPORTS=1`), register dump + guest backtrace (using the .so's symbols) on faults, and `addr2sym`.
7. **docs/CONTRACTS.md**: the exact public API for T4–T7 (memory, HLE registration, marshalling, call_guest, threads, JNI hooks) with examples. **Write it early** (first commit) and tell the coordinator with `ask` when it is ready, because T4–T7 start from it; afterwards only compatible changes.

## Deliverables
`Zombiewood/src/loader/`, `src/cpu/`, `docs/CONTRACTS.md`, tests in `tests/cpu/` and `tests/loader/`.

## Tests (proof)
- Unit tests with small hand-assembled ARM/Thumb blobs (or a test `.so` built with VS's clang `--target=armv7a-linux-androideabi` if available): an import call with int/float/double/variadic args, a host→guest→host callback, two threads using LDREX/STREX.
- `zombiewood --load-only <path to .so>`: loads `libZombieWood.so`, applies relocations, lists imports without a handler, runs `.init_array` and `JNI_OnLoad` with a minimal (stub) `JavaVM*` without crashing, or details where it stops. Paste the output.

## Constraints
Edit only `src/loader/`, `src/cpu/`, `tests/cpu/`, `tests/loader/`, `docs/CONTRACTS.md`, and those folders' CMake. To change the root CMake or the frontend, `ask`. Commits `zombiewood(T3): ...`, no push.
