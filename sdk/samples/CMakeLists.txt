# 示例派生插件的独立构建（不需要 MOD 的源码）：
#     cmake -S . -B build -A x64
#     cmake --build build --config Release
# 生成的 DLL 在 build\Release\，复制到 version.dll 旁的 PBZPlugins\ 文件夹即可（PLUGIN_API.md「快速开始」）。
# 自己的插件照着写一个 add_library(... SHARED ...) 就行；头文件只有 ../include/pbz_combo_api.h 一个。
cmake_minimum_required(VERSION 3.20)
project(pbz_combo_plugin_samples LANGUAGES CXX)
if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "派生插件必须是 x64 DLL：配置时加 -A x64")
endif()
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(MSVC)
    # 源码里的中文字符串按 UTF-8 编译（接口的字符串一律 UTF-8）；静态运行库，玩家不必另装 VC++ 运行库。
    add_compile_options(/utf-8 /W4 /EHsc)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/../include)
add_library(pbz_conditions SHARED conditions/pbz_conditions.cpp)
add_library(pbz_weapon_actions SHARED weapon_actions/pbz_weapon_actions.cpp)
add_library(pbz_mod_panel SHARED mod_panel/pbz_mod_panel.cpp)
