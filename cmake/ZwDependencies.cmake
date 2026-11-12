# Dependencias: vcpkg (SDL2, ANGLE, nlohmann-json, boost) + FetchContent fijado (ImGui, dynarmic).
find_package(SDL2 CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)
find_package(unofficial-angle CONFIG REQUIRED)
find_package(Boost REQUIRED)

include(FetchContent)
set(FETCHCONTENT_QUIET OFF)

# ---- Dear ImGui (fuentes + backends SDL2 / OpenGL3 compilados por nosotros, GLSL ES 100) ----
FetchContent_Declare(imgui_src
  GIT_REPOSITORY https://github.com/ocornut/imgui.git
  GIT_TAG        v1.91.9
  GIT_SHALLOW    TRUE)
FetchContent_GetProperties(imgui_src)
if(NOT imgui_src_POPULATED)
  FetchContent_Populate(imgui_src)
endif()
add_library(zw_imgui STATIC
  ${imgui_src_SOURCE_DIR}/imgui.cpp
  ${imgui_src_SOURCE_DIR}/imgui_draw.cpp
  ${imgui_src_SOURCE_DIR}/imgui_tables.cpp
  ${imgui_src_SOURCE_DIR}/imgui_widgets.cpp
  ${imgui_src_SOURCE_DIR}/imgui_demo.cpp
  ${imgui_src_SOURCE_DIR}/backends/imgui_impl_sdl2.cpp
  ${imgui_src_SOURCE_DIR}/backends/imgui_impl_opengl3.cpp)
target_include_directories(zw_imgui PUBLIC ${imgui_src_SOURCE_DIR} ${imgui_src_SOURCE_DIR}/backends)
target_compile_definitions(zw_imgui PUBLIC IMGUI_IMPL_OPENGL_ES2)
target_link_libraries(zw_imgui PUBLIC SDL2::SDL2 unofficial::angle::libGLESv2 unofficial::angle::libEGL)

# ---- dynarmic (frontend A32, backend x64), commit fijado ----
set(DYNARMIC_FRONTENDS "A32" CACHE STRING "" FORCE)
set(DYNARMIC_TESTS OFF CACHE BOOL "" FORCE)
set(DYNARMIC_WARNINGS_AS_ERRORS OFF CACHE BOOL "" FORCE)
set(DYNARMIC_USE_BUNDLED_EXTERNALS ON CACHE BOOL "" FORCE)
set(DYNARMIC_USE_PRECOMPILED_HEADERS OFF CACHE BOOL "" FORCE)
FetchContent_Declare(dynarmic
  GIT_REPOSITORY https://github.com/azahar-emu/dynarmic.git
  GIT_TAG        a46601580d5512d324104f985b5f0209dc980ddc
  GIT_SUBMODULES_RECURSE TRUE)
# dynarmic y sus externals (mcl, xbyak, zydis, fmt...) se compilan optimizados tambien en Debug: el JIT
# sin optimizar hace el arranque del guest varias veces mas lento. Solo cambian /Od /RTC1 -> /O2 /Ob2;
# el CRT sigue siendo el de depuracion (/MDd, _DEBUG, mismo _ITERATOR_DEBUG_LEVEL), asi que enlaza con
# nuestro codigo, que sigue en /Od. Las variables se restauran tras add_subdirectory (cada directorio
# toma una copia al crearse). ZW_OPTIMIZE_DEPS_IN_DEBUG=OFF lo desactiva (para depurar dynarmic).
option(ZW_OPTIMIZE_DEPS_IN_DEBUG "Compilar dynarmic optimizado en la config Debug" ON)
set(_zw_saved_c_debug "${CMAKE_C_FLAGS_DEBUG}")
set(_zw_saved_cxx_debug "${CMAKE_CXX_FLAGS_DEBUG}")
if(MSVC AND ZW_OPTIMIZE_DEPS_IN_DEBUG)
  foreach(_v CMAKE_C_FLAGS_DEBUG CMAKE_CXX_FLAGS_DEBUG)
    string(REGEX REPLACE "/RTC[1csu]+" "" ${_v} "${${_v}}")
    string(REGEX REPLACE "/O[d012xs]" "" ${_v} "${${_v}}")
    string(REGEX REPLACE "/Ob[0-3]" "" ${_v} "${${_v}}")
    set(${_v} "${${_v}} /O2 /Ob2")
  endforeach()
endif()
FetchContent_MakeAvailable(dynarmic)
set(CMAKE_C_FLAGS_DEBUG "${_zw_saved_c_debug}")
set(CMAKE_CXX_FLAGS_DEBUG "${_zw_saved_cxx_debug}")

# Parche local a dynarmic (T3): su manejador de excepciones de Windows llama al callback de fastmem
# reservando solo 8 bytes de pila; la ABI x64 exige 32 de "shadow space" (+8 de alineacion). Con MSVC
# el callee escribe en ese espacio y pisa la direccion de retorno del manejador -> crash en el primer
# fallo de fastmem. Idempotente; se aplica en cada configure sobre el arbol ya descargado.
set(_zw_dyn_eh "${dynarmic_SOURCE_DIR}/src/dynarmic/backend/x64/exception_handler_windows.cpp")
if(EXISTS "${_zw_dyn_eh}")
  file(READ "${_zw_dyn_eh}" _zw_dyn_eh_src)
  string(FIND "${_zw_dyn_eh_src}" "code.sub(code.rsp, 8);" _zw_dyn_eh_pos)
  if(NOT _zw_dyn_eh_pos EQUAL -1)
    string(REPLACE "code.sub(code.rsp, 8);" "code.sub(code.rsp, 0x28);  // zombiewood: shadow space" _zw_dyn_eh_src "${_zw_dyn_eh_src}")
    string(REPLACE "code.add(code.rsp, 8);" "code.add(code.rsp, 0x28);  // zombiewood: shadow space" _zw_dyn_eh_src "${_zw_dyn_eh_src}")
    file(WRITE "${_zw_dyn_eh}" "${_zw_dyn_eh_src}")
    message(STATUS "dynarmic: parche de shadow space aplicado a exception_handler_windows.cpp")
  else()
    string(FIND "${_zw_dyn_eh_src}" "code.sub(code.rsp, 0x28);" _zw_dyn_eh_pos)
    if(_zw_dyn_eh_pos EQUAL -1)
      message(FATAL_ERROR "dynarmic: no se reconoce exception_handler_windows.cpp; revisar el parche de fastmem")
    endif()
  endif()
else()
  message(FATAL_ERROR "dynarmic: falta ${_zw_dyn_eh}")
endif()
