# astroduel2-overlay

In-game overlay DLL for **Astro Duel 2** (WREngine / cocos2d-x 3.17.x fork,
SDL3). The overlay ships as a **`steam_api64.dll` proxy**: it replaces the
game's Steam DLL, so the game loads it at import resolution — no
launcher, no process injection, just a normal Steam launch.

- Loaded into the Steam-launched game at the main menu.
- Worker thread (spawned from `DllMain`) waits for the game to settle, then
  IAT-hooks `SDL_GL_SwapWindow` (IAT slot located by scanning the known IAT
  region for the live `SDL3.dll` export address — the game's PE import data
  is packer-forged, so no PE parsing).
- The hook runs on the **game's render thread** — the right place for later
  direct-call wiring (no cross-thread game-state races).
- ImGui is lazy-initialized on first swap; desktop GL backend with the
  bundled loader and `#version 120` GLSL for the game's GL 2.1-compat context.
- Input: manual Win32 (GetCursorPos/ScreenToClient + GetAsyncKeyState), no
  WndProc subclassing in v1.
- **Toggle: `Ctrl+Alt+U`.**
- Diagnostics: `%LOCALAPPDATA%\AstroDuel2Mod\probe_ui.log`

## Build

Prereqs: Visual Studio 2022 (C++ workload + CMake components, x64).
CMake ≥ 3.25 — the copy bundled in VS 2022 is fine.

Third-party dependencies come from [vcpkg](https://github.com/microsoft/vcpkg) in
manifest mode — see `vcpkg.json`.

Outputs: `build\Release\ad2ui.dll` and `build\Release\steam_api64.dll`
(the same code; the Steam DLL export table comes from `src/steam_forwards.cpp`,
built with `/G4` to match Steam's x64 ABI guard pages).

## Use

1. Back up the stock DLL: `D:\...\Astro Duel 2\steam_api64.dll`
2. Copy `build\Release\steam_api64.dll` over it
3. Launch the game through Steam as usual

Then toggle with `Ctrl+Alt+U` in-game. (Restore the backup to go back to
stock behavior.)

## Layout

- `src/mod.cpp` — the overlay (IAT + MinHook hooks, ImGui init/UI, diagnostics)
- `src/steam_forwards.cpp` — Steam export forwards for the `steam_api64.dll` proxy
- `vcpkg.json` — dependency manifest

## LLM Disclaimer
An LLM was used to generate code for the initial prototype, and for symbol naming in 
the game's disassembly. All LLM usage was done with a locally running model (Qwen 3.8 27B).
The initial prototype was completed with commit `c112a3b6a9fd39349880c1814f1d26fb7b8be8e1`,
and all programming since has been done by hand.
