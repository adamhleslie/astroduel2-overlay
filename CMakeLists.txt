# astroduel2-overlay -- in-game ImGui overlay DLL + injector for Astro Duel 2
cmake_minimum_required(VERSION 3.25)
project(astroduel2-overlay LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# imgui v1.90.9 (git submodule at imgui/). No top-level CMakeLists upstream,
# so declare the pieces we compile here: core + OpenGL3 backend (bundled
# gl3w-stripped loader; the game's GL 2.1-compat context is handled by the
# backend with "#version 120" shaders).
add_library(imgui STATIC
    imgui/imgui.cpp
    imgui/imgui_draw.cpp
    imgui/imgui_tables.cpp
    imgui/imgui_widgets.cpp
    imgui/backends/imgui_impl_opengl3.cpp)
target_include_directories(imgui PUBLIC imgui imgui/backends)

add_library(ad2ui SHARED
    ad2ui.cpp)   # WIN32_LEAN_AND_MEAN / NOMINMAX are #defined in-source
target_compile_options(ad2ui PRIVATE /W3 /utf-8)
target_link_libraries(ad2ui PRIVATE imgui opengl32 user32 gdi32 kernel32)

add_executable(inject
    inject.c)   # WIN32_LEAN_AND_MEAN is #defined in-source
target_compile_options(inject PRIVATE /W3)
target_link_libraries(inject PRIVATE kernel32)

add_executable(ad2launch
    ad2launch.c)   # WIN32_LEAN_AND_MEAN is #defined in-source
target_compile_options(ad2launch PRIVATE /W3)
target_link_libraries(ad2launch PRIVATE kernel32)
