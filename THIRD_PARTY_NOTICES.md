# References and third-party notices

## UUEPBS — XTGMods

Project: https://github.com/XTGMods/UUEPBS

Reviewed revision: `7c54929929086413afbd12070962f0d499e90c7e` (2026-11-09, checked again 2026-11-13).

Since 2026-11-13 the 身材 / 骨骼 page is the UUEPBS editor itself: **Universal Unreal Engine Player Body Sliders** by **XTGMods** (its README also credits Claude and Dear ImGui). This package contains:

- the UUEPBS v1.3 C++ editor and data layer (`src/uuepbs/`, built into `version.dll`), as ported by a contributor; only `win/slider_window.cpp` was changed (its include paths and hook status now come from this mod);
- that contributor's PBZ bridge (`src/character/npc_body*.cpp`, `npc_bridge.cpp`, `npc_anim.cpp`), which hands the hero's skeleton to the editor and writes its sliders after the pose finalize;
- UUEPBS's own three example presets, unchanged (`samples/UUEPBS Presets/Example - Athletic.json`, `Example - Curvy.json`, `Example - Petite.json`, from `presets/` at the revision above, byte-identical to the repository's blobs; installed next to `version.dll`). Since 2026-11-13 the package ships no other presets and no animations: the presets and animations the contributor had tuned are not included.

It does not bundle UUEPBS binaries, skins, its Lua scripts or UE4SS.

License status, as found on 2026-11-13: the repository has no LICENSE file and GitHub detects no license. Its README ends with the Dear ImGui MIT notice under the heading "Dear ImGui" and the line "UUEPBS links third-party code under this license". The maintainer of this mod read that notice as permission to modify and redistribute UUEPBS and decided on 2026-11-13 to integrate it. This file records that decision; it is not a license granted by XTGMods. If XTGMods publishes a license, its full text and notices replace this paragraph. See `licenses/UUEPBS-LICENSE-STATUS.txt`.

## Dear ImGui — Omar Cornut

Project: https://github.com/ocornut/imgui, version 1.92.1 (`src/third_party/imgui/`), used by the UUEPBS editor window. MIT License, Copyright (c) 2014-2025 Omar Cornut: full text in `licenses/DearImGui-LICENSE.txt`.

## stb_image — Sean Barrett

`src/uuepbs/third_party/stb_image.h` (image decoding for the editor's skins): public domain or MIT, at the user's choice, as its own header states.

## Lua — Lua.org, PUC-Rio

Project: https://www.lua.org, version 5.4.7 (`src/third_party/lua/`), the interpreter behind the Lua scripts of the plugin API (`PBZPlugins\scripts\*.lua`, `src/plugin/lua_host.cpp`). MIT License, Copyright (C) 1994-2024 Lua.org, PUC-Rio: full text in `licenses/Lua-LICENSE.txt`. The sources were taken from the UE4SS fork's copy (`RE-UE4SS/deps/first/LuaRaw`, a repackaging by Narknon, MIT; its licence file is included in the same text file) without changes, except that `lua.c`, `luac.c` and the fork's `luauser.c` (a locking shim) are not included. The build compiles them as C++ and leaves out the libraries a script must not have (`io`, `os`, `package` / `loadlib`, `debug`, `linit`): see `CMakeLists.txt` (`PBZ_LUA_SOURCES`).
