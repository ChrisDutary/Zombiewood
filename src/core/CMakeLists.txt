add_library(zw_core STATIC log.cpp paths.cpp config.cpp)
target_include_directories(zw_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(zw_core PUBLIC nlohmann_json::nlohmann_json PRIVATE shell32)
