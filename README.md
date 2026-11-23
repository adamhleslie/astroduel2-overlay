# astroduel2-overlay

In-game overlay mod for [**Astro Duel 2**](https://store.steampowered.com/app/655350/Astro_Duel_2/) on Steam, Windows x64.

### Features:
- Custom level loading (Versus only)
- Level pool management (Versus only)
  - Custom level selection per-world
  - New random level selection modes 

## Installation
TODO: Installer .ps1 script

## Usage
TODO: Demo gifs / screengrabs
- Add custom .tmx level files to the existing resources folder location: `<AD2 Install Directory>\res\levels\versus\<world>`
- Toggle Hotkey: Backtick / Grave **`**
- Logs: `%LOCALAPPDATA%\AstroDuel2\overlay.log`

## Tech
The overlay ships as a `steam_api64.dll` proxy, replacing the game's Steam DLL, and re-routing
function calls to the original Steam DLL renamed to `steam_api64_real.dll`. A worker thread
spawned from `DllMain` waits for the game to unpack, then hooks the import address table and 
various game functions.

## Build
Prereqs:
- Visual Studio 2022
- CMake ≥ 3.25
- vcpkg (see `vcpkg.json`)
  - Minhook
  - imgui
  - SDL3
  - enum library

## Layout
- `src/steam_forwards.cpp` — Steam export forwards for the `steam_api64.dll` proxy
- `vcpkg.json` — dependency manifest

## LLM Disclaimer
An LLM was used to generate code for the initial prototype, and to generate renamed symbols in 
the Astro Duel 2 disassembly. All LLM usage was done with a locally running model (Qwen 3.8 27B).
All programming since the initial prototype has been done by hand, following commit
`c112a3b6a9fd39349880c1814f1d26fb7b8be8e1`.
