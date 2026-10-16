# astroduel2-overlay

In-game ImGui overlay DLL (`ad2ui.dll`) + injector (`inject.exe`) for
**Astro Duel 2** (WREngine / cocos2d-x 3.17.x fork, SDL3).

Milestone 1 (UI only) — no game calls yet:

- Injected into the Steam-launched game at the main menu.
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

```bat
git clone --recurse-submodules git@github.com:adamhleslie/astroduel2-overlay.git
cd astroduel2-overlay
build_ui.bat            (Release, default)
build_ui.bat Debug      (with PDBs, for stepping through the DLL)
```

Outputs: `build\Release\ad2ui.dll` and `build\Release\inject.exe`.
Line endings are CRLF (`.gitattributes`).

## Use

```bat
:: with the game at the main menu:
build\Release\inject.exe <PID> build\Release\ad2ui.dll
```

Then toggle with `Ctrl+Alt+U` in-game.

## Layout

- `ad2ui.cpp` — the overlay DLL (hook, ImGui init/UI, diagnostics)
- `inject.c` — CreateRemoteThread + LoadLibraryW injector
- `imgui/` — [ocornut/imgui](https://github.com/ocornut/imgui) v1.90.9 (submodule)
