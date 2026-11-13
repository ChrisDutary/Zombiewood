# Instalador de datos (T9): extrae libZombieWood.so, OBB y recursos del APK del usuario.
find_package(ZLIB REQUIRED)  # ya en vcpkg_installed (dependencia de angle)
add_library(zw_installer STATIC installer.cpp)
target_include_directories(zw_installer PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(zw_installer PUBLIC zw_core PRIVATE ZLIB::ZLIB bcrypt)
