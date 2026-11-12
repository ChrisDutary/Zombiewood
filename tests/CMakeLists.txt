add_executable(zw_test_core test_core.cpp)
target_link_libraries(zw_test_core PRIVATE zw_core zw_cpu_deps)
add_test(NAME core_and_cpu_deps COMMAND zw_test_core)
