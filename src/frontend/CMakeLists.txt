# Codigo comun + una implementacion por plataforma de platform.h (dialogos, informe de cierre, icono/cursor
# incrustados) y de video.h (Media Foundation en Windows; en Linux los videos se saltan).
add_library(zw_frontend STATIC window.cpp game.cpp input.cpp png.cpp
                               settings.cpp setup.cpp menu.cpp saves.cpp cursor.cpp)
target_link_libraries(zw_frontend PUBLIC zw_core zw_imgui zw_installer zw_hle_libc SDL2::SDL2
                                  PRIVATE zw_loader zw_cpu zw_hle_gles zw_hle_audio zw_jni zw_gles)
target_include_directories(zw_frontend PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
if(WIN32)
  target_sources(zw_frontend PRIVATE video.cpp platform_win.cpp)
  # mf*: videos; comdlg32: dialogo de abrir archivo; dbghelp: informe de cierre inesperado (MiniDumpWriteDump).
  target_link_libraries(zw_frontend PRIVATE mfplat mfreadwrite mfuuid ole32 comdlg32 dbghelp)
else()
  target_sources(zw_frontend PRIVATE video_linux.cpp platform_linux.cpp)
  target_link_libraries(zw_frontend PRIVATE ${CMAKE_DL_LIBS})

  # Icono y cursor (arte opcional, no versionado) incrustados como datos: lo que en Windows hacen los .rc.
  # zw_embed los convierte en build a un .cpp y escribe ademas el PNG del icono para el AppImage.
  find_package(ZLIB REQUIRED)
  add_executable(zw_embed ${PROJECT_SOURCE_DIR}/tools/embed/zw_embed.cpp)
  target_link_libraries(zw_embed PRIVATE ZLIB::ZLIB)
  set(_zw_ico "${PROJECT_SOURCE_DIR}/resources/zombiewood.ico")
  set(_zw_cur "${PROJECT_SOURCE_DIR}/resources/zombiewood_cursor.bin")
  set(_zw_embed_deps "")
  foreach(_v _zw_ico _zw_cur)
    if(EXISTS "${${_v}}")
      list(APPEND _zw_embed_deps "${${_v}}")
      message(STATUS "Arte incrustado: ${${_v}}")
    else()
      message(STATUS "Sin ${${_v}} (opcional; ver README): se usa el valor por defecto")
      set(${_v} "-")
    endif()
  endforeach()
  set(_zw_embed_cpp "${CMAKE_CURRENT_BINARY_DIR}/zw_embedded.cpp")
  set(ZW_ICON_PNG "${CMAKE_CURRENT_BINARY_DIR}/zombiewood.png" CACHE INTERNAL "Icono PNG para el AppImage")
  add_custom_command(OUTPUT "${_zw_embed_cpp}" "${ZW_ICON_PNG}"
    COMMAND zw_embed "${_zw_cur}" "${_zw_ico}" "${_zw_embed_cpp}" "${ZW_ICON_PNG}"
    DEPENDS zw_embed ${_zw_embed_deps}
    COMMENT "Incrustando icono y cursor (zw_embed)")
  target_sources(zw_frontend PRIVATE "${_zw_embed_cpp}")
endif()

add_executable(zombiewood main.cpp)
# App de ventana (sin consola): SDL2main aporta WinMain y llama a main(). Ver platform_win.cpp (AttachConsole).
set_target_properties(zombiewood PROPERTIES WIN32_EXECUTABLE TRUE)
if(NOT WIN32)
  # Exportar los simbolos del ejecutable: backtrace_symbols() del informe de cierre los necesita.
  target_link_options(zombiewood PRIVATE -rdynamic)
endif()

# Icono del exe: solo si el usuario genero resources/zombiewood.ico (tools/make_icon.ps1); no se versiona.
set(ZW_ICON "${PROJECT_SOURCE_DIR}/resources/zombiewood.ico")
if(WIN32 AND EXISTS "${ZW_ICON}")
  target_sources(zombiewood PRIVATE "${PROJECT_SOURCE_DIR}/resources/zombiewood.rc")
  set_property(SOURCE "${PROJECT_SOURCE_DIR}/resources/zombiewood.rc" APPEND PROPERTY OBJECT_DEPENDS "${ZW_ICON}")
  message(STATUS "Icono de la app: ${ZW_ICON}")
else()
  message(STATUS "Sin icono de la app (genera resources/zombiewood.ico con tools/make_icon.ps1)")
endif()

# Cursor zombi (v0.2.0): solo si existe resources/zombiewood_cursor.bin (tools/make_cursor.ps1); no se versiona.
# Sin el, el juego usa el cursor normal de Windows.
set(ZW_CURSOR "${PROJECT_SOURCE_DIR}/resources/zombiewood_cursor.bin")
if(WIN32 AND EXISTS "${ZW_CURSOR}")
  target_sources(zombiewood PRIVATE "${PROJECT_SOURCE_DIR}/resources/cursor.rc")
  set_property(SOURCE "${PROJECT_SOURCE_DIR}/resources/cursor.rc" APPEND PROPERTY OBJECT_DEPENDS "${ZW_CURSOR}")
  message(STATUS "Cursor del juego: ${ZW_CURSOR}")
else()
  message(STATUS "Sin cursor del juego (genera resources/zombiewood_cursor.bin con tools/make_cursor.ps1)")
endif()
target_link_libraries(zombiewood PRIVATE zw_frontend zw_loader zw_cpu zw_hle_libc zw_hle_gles zw_hle_audio zw_jni zw_gles)
if(TARGET SDL2::SDL2main)
  target_link_libraries(zombiewood PRIVATE SDL2::SDL2main)
endif()

if(NOT WIN32)
  return()  # lo que sigue (copiar las DLL de ANGLE) es solo para Windows
endif()

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
