# zw_hle_gles: 99 imports gl* de libGLESv2 -> ANGLE (T5).
add_library(zw_hle_gles STATIC gles.cpp texdecode.cpp)
target_link_libraries(zw_hle_gles PUBLIC zw_core zw_cpu
                                  PRIVATE unofficial::angle::libGLESv2 unofficial::angle::libEGL)
target_include_directories(zw_hle_gles PUBLIC ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/../..)

# Tests de T5 (fuentes en tests/hle_gles/): contexto ANGLE pbuffer offscreen.
if(BUILD_TESTING)
  set(_zw_tgles ${CMAKE_SOURCE_DIR}/tests/hle_gles)
  add_executable(zw_test_gles ${_zw_tgles}/test_gles.cpp)
  target_link_libraries(zw_test_gles PRIVATE zw_hle_gles unofficial::angle::libGLESv2 unofficial::angle::libEGL)
  # ANGLE: copiar libEGL/libGLESv2 (+ z.dll) junto al test.
  set(_dlls "")
  foreach(_t libEGL libGLESv2)
    set(_loc "")
    foreach(_prop IMPORTED_LOCATION_RELEASE IMPORTED_LOCATION_DEBUG IMPORTED_LOCATION)
      if(NOT _loc)
        get_target_property(_loc unofficial::angle::${_t} ${_prop})
      endif()
    endforeach()
    list(APPEND _dlls "${_loc}")
  endforeach()
  list(GET _dlls 0 _first)
  get_filename_component(_angle_bin "${_first}" DIRECTORY)
  if(EXISTS "${_angle_bin}/z.dll")
    list(APPEND _dlls "${_angle_bin}/z.dll")
  endif()
  add_custom_command(TARGET zw_test_gles POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${_dlls} "$<TARGET_FILE_DIR:zw_test_gles>")
  add_test(NAME hle_gles COMMAND zw_test_gles)
endif()
