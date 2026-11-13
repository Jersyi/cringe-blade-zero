# src/uuepbs — the UUEPBS editor

UUEPBS (Universal Unreal Engine Player Body Sliders) v1.3 by XTGMods, https://github.com/XTGMods/UUEPBS, as a
contributor ported it into this mod (2026-11). The 身材 / 骨骼 page opens this editor; `src/character/npc_bridge.cpp`
connects it to the hero's skeleton and `src/character/npc_body.cpp` writes its sliders through BoneMotionRuntime's
pose hook (shared with the 骨骼变换 node).

Changed from v1.3: `win/slider_window.cpp` only (include paths, the hook status comes from `NpcBody`). Built at its own
warning level (CMakeLists.txt, `PBZ_VENDOR_SOURCES`). License status: `THIRD_PARTY_NOTICES.md` and
`licenses/UUEPBS-LICENSE-STATUS.txt`.
