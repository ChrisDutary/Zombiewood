# zw_loader: loader ELF32 ARM + modo --load-only con HLE minima de arranque (T3).
add_library(zw_loader STATIC loader.cpp load_only.cpp bootstrap_hle.cpp)
target_link_libraries(zw_loader PUBLIC zw_core zw_cpu)
target_include_directories(zw_loader PUBLIC ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/..)

# Ejecutable de diagnostico independiente del frontend (equivale a `zombiewood --load-only`).
add_executable(zw_loadonly loadonly_main.cpp)
target_link_libraries(zw_loadonly PRIVATE zw_loader)

if(BUILD_TESTING)
  set(_zw_tld ${CMAKE_SOURCE_DIR}/tests/loader)
  if(EXISTS ${_zw_tld}/test_loader.cpp)
    add_executable(zw_test_loader ${_zw_tld}/test_loader.cpp)
    target_include_directories(zw_test_loader PRIVATE ${_zw_tld})
    target_link_libraries(zw_test_loader PRIVATE zw_loader)
    add_test(NAME loader COMMAND zw_test_loader)
  endif()
endif()
