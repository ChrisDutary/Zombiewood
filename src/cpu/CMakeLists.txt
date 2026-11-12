# zw_cpu_deps: dynarmic (A32 + backend x64) compilado y enlazado.
add_library(zw_cpu_deps STATIC deps_check.cpp)
target_link_libraries(zw_cpu_deps PUBLIC dynarmic)
target_include_directories(zw_cpu_deps PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)

# zw_cpu: memoria guest, registro HLE, CPU/JIT, call_guest, hilos, depuracion (T3).
add_library(zw_cpu STATIC guest_memory.cpp hle.cpp cpu.cpp)
target_link_libraries(zw_cpu PUBLIC zw_core zw_cpu_deps)
target_include_directories(zw_cpu PUBLIC ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/..)

# Tests de T3 (fuentes en tests/cpu/).
if(BUILD_TESTING)
  set(_zw_tcpu ${CMAKE_SOURCE_DIR}/tests/cpu)
  add_executable(zw_test_cpu ${_zw_tcpu}/test_cpu.cpp)
  target_include_directories(zw_test_cpu PRIVATE ${_zw_tcpu})
  target_link_libraries(zw_test_cpu PRIVATE zw_cpu)
  add_test(NAME cpu COMMAND zw_test_cpu)
endif()
