#pragma once
namespace zw {
// Crea un Jit A32 de dynarmic con memoria plana y ejecuta "mov r0,#42; b ."; devuelve r0 (42 si todo ok).
int CpuDepsSelfTest();
}  // namespace zw
