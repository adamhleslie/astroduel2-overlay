# astroduel2-overlay - in-game ImGui overlay DLL (steam_api64.dll proxy) for Astro Duel 2
cmake_minimum_required(VERSION 3.25)
project(astroduel2-overlay VERSION 1.0.0 LANGUAGES C CXX)
set(project_version_suffix "-beta2")
set(project_name_versioned "AstroDuel2-Overlay_${PROJECT_VERSION}${project_version_suffix}")

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

# TODO: Add VERSIONINFO resource script https://learn.microsoft.com/en-us/windows/win32/menurc/versioninfo-resource
# Overlay -> steam_api64 proxy
add_library(steam_api64 SHARED
    src/dllmain.cpp
    src/mod.cpp
    src/ad2.cpp
    src/steam_forwards.cpp
    src/hooks_iat.cpp
    src/hooks_game.cpp
    src/level_pool.cpp
    src/overlay.cpp
    src/ad2_iat.cpp
)
target_compile_options(steam_api64 PRIVATE /W4 /utf-8)
target_compile_options(steam_api64 PRIVATE $<$<CONFIG:RelWithDebInfo>:-UNDEBUG>) # Remove NDEBUG compile definition in RelWithDebInfo for assert support
target_compile_definitions(steam_api64 PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN) # Windows header include options
target_link_libraries(steam_api64 PRIVATE imgui::imgui minhook::minhook enchantum::enchantum SDL3::SDL3 opengl32 user32 gdi32 shell32 kernel32)

# Deploy + launch based on GAME_DIR being set to the AstroDuel2 directory
if (GAME_DIR)
    add_custom_target(deploy
        DEPENDS steam_api64
        COMMAND pwsh -NoProfile -File "${CMAKE_CURRENT_SOURCE_DIR}/tools/CopyAndRun.ps1" "${GAME_DIR}" -Configuration $<CONFIG> -NoRun
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
        USES_TERMINAL
        COMMENT "Deploying steam_api64.dll to the game directory")
else ()
    message(WARNING "GAME_DIR is not set - the 'deploy' target is disabled.")
endif ()

# Install into local directory for packaging, unless user specified location
if (CMAKE_INSTALL_PREFIX_INITIALIZED_TO_DEFAULT)
    set(CACHE{CMAKE_INSTALL_PREFIX} TYPE PATH HELP "Install prefix" FORCE VALUE "${CMAKE_CURRENT_SOURCE_DIR}/install")
endif ()

install(TARGETS steam_api64 RUNTIME DESTINATION "package" CONFIGURATIONS Release)
install(PROGRAMS "tools/packaging/Install.ps1" DESTINATION "package" CONFIGURATIONS Release)

# Archive contents of the package directory
install(CODE "set(project_name_versioned ${project_name_versioned})")
install(CODE [[
    if ($<CONFIG:Release>)
        file(SHA256 "${CMAKE_INSTALL_PREFIX}/package/steam_api64.dll" steam_api64.dll_sha256)
        file(SHA256 "${CMAKE_INSTALL_PREFIX}/package/Install.ps1" Install.ps1_sha256)
        string(TOUPPER ${steam_api64.dll_sha256} steam_api64.dll_sha256)
        string(TOUPPER ${Install.ps1_sha256} Install.ps1_sha256)
        configure_file("../tools/packaging/Install.md" "${CMAKE_INSTALL_PREFIX}/package/README.md" NEWLINE_STYLE CRLF)
        file(GLOB archive_files LIST_DIRECTORIES true RELATIVE "${CMAKE_INSTALL_PREFIX}/package" "${CMAKE_INSTALL_PREFIX}/package/*")
        file(ARCHIVE_CREATE
            OUTPUT "${CMAKE_INSTALL_PREFIX}/${project_name_versioned}.zip"
            PATHS ${archive_files}
            WORKING_DIRECTORY "${CMAKE_INSTALL_PREFIX}/package"
            FORMAT zip
        )
    else ()
        message(WARNING "Install not supported on non-Release build")
    endif ()
]])
