# astroduel2-overlay -- in-game ImGui overlay DLL (steam_api64.dll proxy) for Astro Duel 2
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

# MinHook v1.3.4 (submodule at minhook/, BSD-2-Clause): inline function
# hooking with HDE64-decoded trampolines. Used for the entry hooks on
# WorldDataLoader::worldDataForType (RVA 0x2ae190) and the level-string
# composer (RVA 0x26C1C0). Compiled as C like upstream; x64-only (hde64).
add_library(minhook STATIC
    minhook/src/hook.c
    minhook/src/buffer.c
    minhook/src/trampoline.c
    minhook/src/hde/hde64.c)
target_include_directories(minhook PUBLIC minhook/include)

# cJSON v1.7.19 (submodule at cjson/, zlib license): the parser for
# res/levels/versus/pool.json (the per-world level-count override table).
# One translation unit, C89.
add_library(cjson STATIC
    cjson/cJSON.c)
target_include_directories(cjson PUBLIC cjson)

add_library(ad2ui SHARED
    ad2ui.cpp)   # WIN32_LEAN_AND_MEAN / NOMINMAX are #defined in-source
# /Oy- (disable frame-pointer omission) so _ReturnAddress() in the
# hooks' ra0 caller forensics (_ReturnAddress, below) is reliable.
# Negligible cost for this DLL.
target_compile_options(ad2ui PRIVATE /W3 /utf-8 /Oy-)
target_link_libraries(ad2ui PRIVATE imgui minhook cjson opengl32 user32 gdi32 shell32 kernel32)

# steam_api64 proxy: same overlay code, but built as the game's own
# steam_api64.dll and loaded at import resolution (before any game code
# runs). steam_forwards.cpp carries 1096 linker /export pragmas forwarding
# every real export to steam_api64_real.dll (the renamed original),
# ordinals preserved. Pragmas instead of a .def: MSVC's .def forwarder
# check (a known linker bug) would otherwise require the target DLL at
# link time. Regenerate after game updates:
#   tools/generate_steam_proxy.py
add_library(steam_api64 SHARED
    ad2ui.cpp
    steam_forwards.cpp)
# /Oy-: same as ad2ui -- ad2ui.cpp uses _ReturnAddress() RA forensics
target_compile_options(steam_api64 PRIVATE /W3 /utf-8 /Oy-)
target_link_libraries(steam_api64 PRIVATE imgui minhook cjson opengl32 user32 gdi32 shell32 kernel32)

