# zw_cpu_deps: dynarmic (A32 + backend x64) compilado y enlazado; T3 construye zw_cpu encima.
add_library(zw_cpu_deps STATIC deps_check.cpp)
target_link_libraries(zw_cpu_deps PUBLIC dynarmic)
target_include_directories(zw_cpu_deps PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)

add_library(zw_cpu STATIC stub.cpp)
target_link_libraries(zw_cpu PUBLIC zw_core zw_cpu_deps)
target_include_directories(zw_cpu PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
