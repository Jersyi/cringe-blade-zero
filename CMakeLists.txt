cmake_minimum_required(VERSION 3.24)
project(PBZDebugMenu LANGUAGES CXX ASM_MASM)
set(CMAKE_CXX_STANDARD 17)
add_compile_options($<$<COMPILE_LANGUAGE:CXX>:/utf-8>)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# Every source folder is an include root, so headers are included by bare name.
include_directories(src/core src/ui src/boss src/grab src/air src/weapons src/character src/hair src/pad src/supplement src/generated)

function(pbz_settings target)
    target_compile_definitions(${target} PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
    target_compile_options(${target} PRIVATE "$<$<COMPILE_LANGUAGE:CXX>:/W4;/WX;/EHsc>")
endfunction()

add_library(version SHARED
    src/core/proxy.cpp
    src/ui/debug_hub.cpp src/ui/ui_i18n.cpp src/ui/ui_theme.cpp src/ui/ui_glass.cpp src/ui/ui_load_banner.cpp
    src/ui/ui_host.cpp src/ui/ui_hotkeys.cpp src/ui/ui_cursor.cpp src/ui/ui_assets.cpp
    src/hair/hair_controls.cpp
    src/boss/boss_menu.cpp src/boss/boss_rush.cpp src/boss/boss_record_guard.cpp src/boss/boss_ai.cpp src/boss/boss_ai_menu.cpp
    src/boss/entry_safety.cpp src/boss/entry_protection.cpp src/boss/entry_focus.cpp src/boss/test_dummy.cpp
    src/character/character_swap.cpp src/character/npc_retarget.cpp src/character/npc_menu.cpp src/character/npc_combo.cpp src/character/combo_file.cpp src/character/combo_menu.cpp
    src/air/air_skills.cpp src/air/air_skills_menu.cpp
    src/grab/motian_grab.cpp src/grab/motian_grab_menu.cpp src/grab/grab_extras.cpp
    src/core/settings_store.cpp src/ui/hub_pause.cpp src/ui/gear_widget.cpp src/ui/skill_toast.cpp src/core/title_control.cpp
    src/weapons/weapon_presets.cpp src/weapons/weapon_icons.cpp src/weapons/weapon_presets_menu.cpp
    src/pad/pad_triggers.cpp src/core/ue_runtime.cpp src/core/game_thread.cpp src/core/game_build.cpp src/hair/groom_force.cpp
    src/supplement/debug_supplement.cpp src/supplement/supplement_menu.cpp src/ui/donate_page.cpp
    src/core/forwarders.asm src/core/version.def)
pbz_settings(version)
target_link_libraries(version PRIVATE bcrypt user32 gdi32 comctl32 cabinet)
set_target_properties(version PROPERTIES OUTPUT_NAME version)

# Standalone version.dll: adaptive triggers always off, nothing else (no debug menu / panel / ini), in build/triggers_only.
add_library(version_triggers SHARED src/triggers_only/dllmain.cpp src/pad/pad_triggers.cpp src/core/forwarders.asm src/triggers_only/version.def)
pbz_settings(version_triggers)
set_target_properties(version_triggers PROPERTIES OUTPUT_NAME version
    RUNTIME_OUTPUT_DIRECTORY_RELEASE ${CMAKE_BINARY_DIR}/triggers_only RUNTIME_OUTPUT_DIRECTORY_DEBUG ${CMAKE_BINARY_DIR}/triggers_only_debug
    ARCHIVE_OUTPUT_DIRECTORY_RELEASE ${CMAKE_BINARY_DIR}/triggers_only ARCHIVE_OUTPUT_DIRECTORY_DEBUG ${CMAKE_BINARY_DIR}/triggers_only_debug)

# Tests
add_executable(proxy_test tests/proxy_test.cpp)
pbz_settings(proxy_test)

add_executable(hair_controls_test tests/hair_controls_test.cpp
    src/hair/hair_controls.cpp src/ui/debug_hub.cpp src/boss/entry_focus.cpp src/ui/ui_i18n.cpp src/ui/ui_theme.cpp src/ui/ui_glass.cpp
    src/ui/ui_load_banner.cpp src/ui/ui_host.cpp src/ui/ui_hotkeys.cpp src/ui/ui_cursor.cpp src/ui/ui_assets.cpp src/pad/pad_triggers.cpp
    src/core/ue_runtime.cpp src/hair/groom_force.cpp src/core/settings_store.cpp src/ui/hub_pause.cpp)
pbz_settings(hair_controls_test)
target_link_libraries(hair_controls_test PRIVATE user32 gdi32 comctl32)

# Game input path: fake UnrealWindow thread + real hub worker; counts where injected keys land.
add_executable(input_focus_test WIN32 tests/input_focus_test.cpp
    src/ui/debug_hub.cpp src/boss/entry_focus.cpp src/ui/ui_i18n.cpp src/ui/ui_theme.cpp src/ui/ui_glass.cpp src/ui/ui_load_banner.cpp
    src/ui/ui_host.cpp src/ui/ui_hotkeys.cpp src/ui/ui_cursor.cpp src/grab/motian_grab_menu.cpp src/grab/grab_extras.cpp
    src/core/settings_store.cpp src/ui/hub_pause.cpp src/core/ue_runtime.cpp src/ui/gear_widget.cpp src/ui/ui_assets.cpp
    src/weapons/weapon_presets_menu.cpp src/weapons/weapon_icons.cpp src/ui/donate_page.cpp)
pbz_settings(input_focus_test)
target_link_libraries(input_focus_test PRIVATE user32 gdi32 comctl32)

# Tick hook fault recovery and ue:: cache revalidation after garbage collection (map changes).
add_executable(game_thread_test tests/game_thread_test.cpp src/core/game_thread.cpp src/core/ue_runtime.cpp)
pbz_settings(game_thread_test)

# Steam / WeGame self-check bytes: protector-stub rows swapped in only on the build they were made for.
add_executable(game_build_test tests/game_build_test.cpp src/core/game_build.cpp src/core/ue_runtime.cpp)
pbz_settings(game_build_test)

# NPC skin boss skills: attack-collider tags copied onto the hero per skin.
add_executable(npc_skill_colliders_test tests/npc_skill_colliders_test.cpp)
pbz_settings(npc_skill_colliders_test)

# Grab damage per damage event: multiplier / percent extra of each native hit, presets.
add_executable(grab_damage_test tests/grab_damage_test.cpp)
pbz_settings(grab_damage_test)

# NPC retarget two-handed grip: left-arm contact IK on a synthetic hero/NPC skeleton pair.
add_executable(npc_pose_transfer_test tests/npc_pose_transfer_test.cpp)
pbz_settings(npc_pose_transfer_test)

# Debug supplement page: every catalog item on exactly one card.
add_executable(supplement_layout_test tests/supplement_layout_test.cpp)
pbz_settings(supplement_layout_test)

# DualSense adaptive-trigger rewrite (pad_triggers.h): report layouts, Steam vtable hook, import-table path.
add_executable(pad_triggers_test tests/pad_triggers_test.cpp src/pad/pad_triggers.cpp)
pbz_settings(pad_triggers_test)

# 招式派生 model: move kinds, chain plan / slow-down rate, hero dodge families, ini text.
add_executable(npc_combo_test tests/npc_combo_test.cpp)
pbz_settings(npc_combo_test)

# 招式派生 page rendered offline from the generated skill / dodge tables (stubbed game state) to a PNG.
add_executable(combo_menu_preview tests/combo_menu_preview.cpp src/character/combo_menu.cpp src/character/npc_combo.cpp src/character/combo_file.cpp
    src/core/settings_store.cpp src/ui/ui_theme.cpp src/ui/ui_i18n.cpp src/ui/ui_assets.cpp src/ui/ui_host.cpp src/weapons/weapon_icons.cpp)
pbz_settings(combo_menu_preview)
target_link_libraries(combo_menu_preview PRIVATE user32 gdi32 gdiplus comctl32 dwmapi uxtheme cabinet shlwapi)

# .combo files: format round trip, bad files, ini migration, save-as / select, live reload.
add_executable(combo_file_test tests/combo_file_test.cpp src/character/combo_file.cpp src/character/npc_combo.cpp src/core/settings_store.cpp)
pbz_settings(combo_file_test)
target_link_libraries(combo_file_test PRIVATE cabinet)

# EnvCheck.exe: the players' environment check and repair (Install.bat 4; build.ps1 packages it). known_exes.h is made
# from proxy.cpp at build time, so the PBZ.exe builds it accepts have one source (update_port.py rewrites them there).
# Only kernel32 / advapi32 / bcrypt load at start; the rest is delay-loaded, and DEPENDENTLOADFLAG 0x800 keeps static
# imports in System32, so the mod's version.dll next to it is never loaded into it.
set(PBZ_KNOWN_EXES ${CMAKE_BINARY_DIR}/generated/known_exes.h)
add_custom_command(OUTPUT ${PBZ_KNOWN_EXES}
    COMMAND ${CMAKE_COMMAND} -DPROXY=${CMAKE_SOURCE_DIR}/src/core/proxy.cpp -DTEMPLATE=${CMAKE_SOURCE_DIR}/src/env_check/known_exes.h.in
            -DOUT=${PBZ_KNOWN_EXES} -P ${CMAKE_SOURCE_DIR}/src/env_check/known_exes.cmake
    DEPENDS src/core/proxy.cpp src/env_check/known_exes.h.in src/env_check/known_exes.cmake
    COMMENT "known_exes.h from proxy.cpp")
add_custom_target(pbz_known_exes DEPENDS ${PBZ_KNOWN_EXES})
add_executable(EnvCheck src/env_check/env_check.cpp src/env_check/env_checks.cpp src/character/combo_file.cpp)
add_dependencies(EnvCheck pbz_known_exes)
pbz_settings(EnvCheck)
target_include_directories(EnvCheck PRIVATE src/env_check ${CMAKE_BINARY_DIR}/generated)
target_link_libraries(EnvCheck PRIVATE bcrypt advapi32 cabinet setupapi shell32 ole32 urlmon wintrust crypt32 wevtapi delayimp)
target_link_options(EnvCheck PRIVATE /DEPENDENTLOADFLAG:0x800 /DELAYLOAD:cabinet.dll /DELAYLOAD:setupapi.dll /DELAYLOAD:shell32.dll
    /DELAYLOAD:ole32.dll /DELAYLOAD:urlmon.dll /DELAYLOAD:wintrust.dll /DELAYLOAD:crypt32.dll /DELAYLOAD:wevtapi.dll)

# EnvCheck pure parts: PE imports / exports, DLL markers, last-run log, SHA-256, known_exes.h.
add_executable(env_check_test tests/env_check_test.cpp src/env_check/env_checks.cpp)
add_dependencies(env_check_test pbz_known_exes)
pbz_settings(env_check_test)
target_include_directories(env_check_test PRIVATE src/env_check ${CMAKE_BINARY_DIR}/generated)
target_link_libraries(env_check_test PRIVATE bcrypt)
