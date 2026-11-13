# Instalador de datos (T9): extrae libZombieWood.so, OBB y recursos del APK del usuario.
find_package(ZLIB REQUIRED)  # zlib: declarado en vcpkg.json
add_library(zw_installer STATIC installer.cpp)
target_include_directories(zw_installer PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(zw_installer PUBLIC zw_core PRIVATE ZLIB::ZLIB bcrypt)

if(BUILD_TESTING)
  add_executable(zw_test_installer ${PROJECT_SOURCE_DIR}/tests/installer/test_installer.cpp)
  target_link_libraries(zw_test_installer PRIVATE zw_installer zw_core ZLIB::ZLIB)
  add_test(NAME installer COMMAND zw_test_installer)
  # z.dll (zlib de vcpkg) junto al test
  get_target_property(_zlib_loc ZLIB::ZLIB IMPORTED_LOCATION_RELEASE)
  if(NOT _zlib_loc)
    set(_zlib_loc "${ZLIB_LIBRARY_RELEASE}")
  endif()
  get_filename_component(_zlib_bin "${_zlib_loc}" DIRECTORY)
  file(GLOB _zlib_dlls "${_zlib_bin}/../bin/z*.dll")
  if(_zlib_dlls)
    add_custom_command(TARGET zw_test_installer POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_if_different ${_zlib_dlls} $<TARGET_FILE_DIR:zw_test_installer>)
  endif()
endif()
