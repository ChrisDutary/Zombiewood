# Tests de T4 (incluido desde src/hle/libc/CMakeLists.txt).
add_executable(zw_test_libc ${CMAKE_CURRENT_LIST_DIR}/test_libc.cpp)
target_link_libraries(zw_test_libc PRIVATE zw_hle_libc)
add_test(NAME hle_libc COMMAND zw_test_libc)

add_executable(zw_libc_loadonly ${CMAKE_CURRENT_LIST_DIR}/libc_loadonly.cpp)
target_link_libraries(zw_libc_loadonly PRIVATE zw_hle_libc zw_loader)
