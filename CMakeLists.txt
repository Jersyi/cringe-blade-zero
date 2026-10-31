cmake_minimum_required(VERSION 3.24)
project(PBZDebugMenu LANGUAGES CXX ASM_MASM)
set(CMAKE_CXX_STANDARD 17)
add_compile_options($<$<COMPILE_LANGUAGE:CXX>:/utf-8>)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# Every source folder is an include root, so headers are included by bare name.
include_directories(src/core src/ui src/boss src/grab src/air src/weapons src/character src/hair src/pad src/generated)

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
    src/boss/entry_safety.cpp src/boss/entry_protection.cpp src/boss/entry_focus.cpp
    src/character/character_swap.cpp src/character/npc_retarget.cpp src/character/npc_menu.cpp
    src/air/air_skills.cpp src/air/air_skills_menu.cpp
    src/grab/motian_grab.cpp src/grab/motian_grab_menu.cpp src/grab/grab_extras.cpp
    src/core/settings_store.cpp src/ui/hub_pause.cpp src/ui/gear_widget.cpp
    src/weapons/weapon_presets.cpp src/weapons/weapon_icons.cpp src/weapons/weapon_presets_menu.cpp
    src/pad/pad_triggers.cpp src/core/ue_runtime.cpp src/core/game_thread.cpp src/hair/groom_force.cpp
    src/core/forwarders.asm src/core/version.def)
pbz_settings(version)
target_link_libraries(version PRIVATE bcrypt user32 gdi32 comctl32)
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
    src/weapons/weapon_presets_menu.cpp src/weapons/weapon_icons.cpp)
pbz_settings(input_focus_test)
target_link_libraries(input_focus_test PRIVATE user32 gdi32 comctl32)

# Tick hook fault recovery and ue:: cache revalidation after garbage collection (map changes).
add_executable(game_thread_test tests/game_thread_test.cpp src/core/game_thread.cpp src/core/ue_runtime.cpp)
pbz_settings(game_thread_test)

# DualSense adaptive-trigger rewrite (pad_triggers.h): report layouts, Steam vtable hook, import-table path.
add_executable(pad_triggers_test tests/pad_triggers_test.cpp src/pad/pad_triggers.cpp)
pbz_settings(pad_triggers_test)
