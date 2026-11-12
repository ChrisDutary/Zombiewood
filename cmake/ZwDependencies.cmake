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
FetchContent_MakeAvailable(dynarmic)
