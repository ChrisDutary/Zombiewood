# zw_hle_libc: HLE de bionic (T4). Ver libc.h.
add_library(zw_hle_libc STATIC
  register.cpp memory.cpp string.cpp ctype.cpp wchar.cpp format.cpp stdio.cpp vfs.cpp zip.cpp
  posix.cpp time.cpp pthread.cpp runtime.cpp math.cpp dl.cpp log.cpp net.cpp)
target_link_libraries(zw_hle_libc PUBLIC zw_core zw_cpu zw_loader)
target_include_directories(zw_hle_libc PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/../..)

# Tests de T4 (fuentes en tests/hle_libc/).
if(BUILD_TESTING AND EXISTS ${CMAKE_SOURCE_DIR}/tests/hle_libc/CMakeLists.txt)
  add_subdirectory(${CMAKE_SOURCE_DIR}/tests/hle_libc ${CMAKE_BINARY_DIR}/tests/hle_libc)
endif()
