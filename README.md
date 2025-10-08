# ScarToolkit

Utilities and ImGui overlay for **SCAR/Lua scenario scripting** in RTS games.  
Focus: single-player/offline prototyping, debugging, and developer education.

## Features
- In-game overlay (ImGui) with quick actions (spawns, camera helpers, debug).
- Example SCAR/Lua snippets for scenario logic.
- Small utilities for inspecting entities, vision, and territory data.

## Scope / Not in scope
- ✅ Local/offline use, modding and scenario scripting.
- ❌ Multiplayer/online advantage, anti-cheat interaction, or redistributed game assets.

## Build
- **IDE:** Visual Studio (x64), Windows 10/11 SDK, C++17.
- **Graphics:** D3D11 (ImGui backends included).
- Build target: **ScarToolkit** (Debug or Release).

## Run
- Build → run `ScarToolkit.exe` from `x64/Debug` or `x64/Release`.

## Contributing
- Open an issue or PR with small, focused changes.
- Keep binaries out of the repo (build locally). Use Releases for sharing builds.

## License
MIT – no warranty. You’re responsible for how you use it.
