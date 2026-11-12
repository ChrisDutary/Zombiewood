add_library(zw_loader STATIC stub.cpp)
target_link_libraries(zw_loader PUBLIC zw_core)
target_include_directories(zw_loader PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
