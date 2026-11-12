add_library(zw_hle_gles STATIC stub.cpp)
target_link_libraries(zw_hle_gles PUBLIC zw_core)
target_include_directories(zw_hle_gles PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
