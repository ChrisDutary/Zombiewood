add_library(zw_hle_libc STATIC stub.cpp)
target_link_libraries(zw_hle_libc PUBLIC zw_core)
target_include_directories(zw_hle_libc PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
