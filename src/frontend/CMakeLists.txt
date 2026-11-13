add_library(zw_frontend STATIC window.cpp game.cpp input.cpp png.cpp
                               settings.cpp setup.cpp menu.cpp saves.cpp video.cpp)
target_link_libraries(zw_frontend PUBLIC zw_core zw_imgui zw_installer zw_hle_libc SDL2::SDL2
                                  PRIVATE zw_loader zw_cpu zw_hle_gles zw_hle_audio zw_jni
                                          unofficial::angle::libGLESv2
                                          mfplat mfreadwrite mfuuid ole32 comdlg32)
target_include_directories(zw_frontend PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)

add_executable(zombiewood main.cpp)
target_link_libraries(zombiewood PRIVATE zw_frontend zw_loader zw_cpu zw_hle_libc zw_hle_gles zw_hle_audio zw_jni
                                          SDL2::SDL2main)

# ANGLE: copiar libEGL.dll / libGLESv2.dll junto al exe (cargados en runtime por SDL como driver GLES).
foreach(_t libEGL libGLESv2)
  set(_loc "")
  foreach(_prop IMPORTED_LOCATION_RELEASE IMPORTED_LOCATION_DEBUG IMPORTED_LOCATION)
    if(NOT _loc)
      get_target_property(_loc unofficial::angle::${_t} ${_prop})
    endif()
  endforeach()
  if(NOT _loc)
    message(FATAL_ERROR "No se encontro la DLL de unofficial::angle::${_t}")
  endif()
  list(APPEND ZW_ANGLE_DLLS "${_loc}")
endforeach()
# El ANGLE de vcpkg importa z.dll (zlib release) tambien en debug.
list(GET ZW_ANGLE_DLLS 0 _first)
get_filename_component(_angle_bin "${_first}" DIRECTORY)
if(EXISTS "${_angle_bin}/z.dll")
  list(APPEND ZW_ANGLE_DLLS "${_angle_bin}/z.dll")
endif()
message(STATUS "ANGLE DLLs: ${ZW_ANGLE_DLLS}")
add_custom_command(TARGET zombiewood POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_if_different ${ZW_ANGLE_DLLS} "$<TARGET_FILE_DIR:zombiewood>"
  COMMENT "Copiando ANGLE (libEGL/libGLESv2) junto al exe")
