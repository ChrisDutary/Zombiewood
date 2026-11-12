add_library(zw_hle_audio STATIC stub.cpp)
target_link_libraries(zw_hle_audio PUBLIC zw_core)
target_include_directories(zw_hle_audio PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
