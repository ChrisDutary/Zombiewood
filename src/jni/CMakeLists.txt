add_library(zw_jni STATIC stub.cpp)
target_link_libraries(zw_jni PUBLIC zw_core)
target_include_directories(zw_jni PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
