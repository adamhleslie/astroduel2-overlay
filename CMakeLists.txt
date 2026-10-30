# astroduel2-overlay -- in-game ImGui overlay DLL (steam_api64.dll proxy) for Astro Duel 2
cmake_minimum_required(VERSION 3.25)
project(astroduel2-overlay LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# imgui w/ OpenGL3 backend
add_library(imgui STATIC
    external/imgui/imgui.cpp
    external/imgui/imgui_draw.cpp
    external/imgui/imgui_tables.cpp
    external/imgui/imgui_widgets.cpp
    external/imgui/backends/imgui_impl_opengl3.cpp)
target_include_directories(imgui PUBLIC external/imgui external/imgui/backends)

# MinHook
if (NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "Expected 64 bit, but CMAKE_SIZEOF_VOID_P is ${CMAKE_SIZEOF_VOID_P}")
endif ()
add_subdirectory(external/minhook)
if (NOT TARGET minhook)
    message (FATAL_ERROR "minhook library not defined by adding subdirectory external/minhook")
endif ()

# TODO: Re-implement when cjson needed again
#add_subdirectory(external/cjson)
#if (NOT TARGET cjson)
#    message (FATAL_ERROR "cjson library not defined by adding subdirectory external/cjson")
#endif ()

# Overlay + steam_api64 proxy
add_library(steam_api64 SHARED
    src/mod.cpp
    src/steam_forwards.cpp
    src/hooks_iat.cpp
    src/hooks_game.cpp
    src/level_pool.cpp
    src/overlay.cpp)

# TODO: Investigate refactoring to use RtlCaptureStackBackTrace
# /Oy- (disable frame-pointer omission) for _ReturnAddress() reliability
target_compile_options(steam_api64 PRIVATE /W3 /utf-8 /Oy-)
target_link_libraries(steam_api64 PRIVATE imgui minhook opengl32 user32 gdi32 shell32 kernel32)
