@echo off
setlocal
rem astroduel2-overlay -- build with CMake (uses the CMake bundled in VS 2022).
rem Usage:  build_ui.bat [Release|Debug]
cd /d "%~dp0"

set CFG=Release
if not "%~1"=="" set CFG=%~1

rem Locate CMake (>= 3.25): PATH first, then the copy VS 2022 ships with.
set "CMK="
where cmake >nul 2>nul && set "CMK=cmake"
if defined CMK goto :havecmake
set "CMK=C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
if not exist "%CMK%" (
  echo ERROR: cmake not found. Install VS 2022 with the CMake components.
  exit /b 1
)
:havecmake

rem First run needs the imgui submodule:
if not exist "imgui\imgui.h" (
  echo ERROR: imgui missing. Run: git submodule update --init
  exit /b 1
)

"%CMK%" --preset windows || exit /b 1
"%CMK%" --build --preset %CFG% --parallel || exit /b 1

echo.
echo Built %date% %time%:
dir /b "build\%CFG%\ad2ui.dll" "build\%CFG%\steam_api64.dll"
endlocal
