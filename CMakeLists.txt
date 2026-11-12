# astroduel2-overlay -- in-game ImGui overlay DLL (steam_api64.dll proxy) for Astro Duel 2
cmake_minimum_required(VERSION 3.25)
project(astroduel2-overlay LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if (NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "Expected 64 bit, but CMAKE_SIZEOF_VOID_P is ${CMAKE_SIZEOF_VOID_P}")
endif ()

# Find cmake config packages for dependencies
find_package(minhook CONFIG REQUIRED)
find_package(enchantum CONFIG REQUIRED)
find_package(imgui CONFIG REQUIRED)
find_package(SDL3 CONFIG REQUIRED)

# Overlay -> steam_api64 proxy
add_library(steam_api64 SHARED
    src/dllmain.cpp
    src/mod.cpp
    src/ad2.cpp
    src/steam_forwards.cpp
    src/hooks_iat.cpp
    src/hooks_game.cpp
    src/level_pool.cpp
    src/overlay.cpp)

set_property(TARGET steam_api64 PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL") # MSVC DLL
target_compile_options(steam_api64 PRIVATE /W4 /utf-8)
target_compile_definitions(steam_api64 PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN) # Windows header include options
target_link_libraries(steam_api64 PRIVATE imgui::imgui minhook::minhook enchantum::enchantum SDL3::SDL3 opengl32 user32 gdi32 shell32 kernel32)
