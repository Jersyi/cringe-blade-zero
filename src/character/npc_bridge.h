#pragma once
// UUEPBS 的整套界面与数据层（照搬 v1.3），通过这里接进豪之刃：
//   · Feed()   —— 游戏线程每帧喂骨架信息（名字、父子表、姿势 buffer 偏移）与安全读函数
//   · Start()  —— 起它自己的窗口（CPU 渲染，不碰 D3D，不碰游戏的 swap chain）
//   · Toggle() —— 开/关窗口（F10 面板里的按钮调）
#include <cstdint>
#include <string>
#include <vector>

namespace NpcBridge
{
    bool SafeRead(uintptr_t address, void* out, size_t size);

    // 第一次调用会把骨架交出去；换组件或骨数变了会重新交。返回是否已经交给界面层。
    bool Feed(void* component);

    // 引擎刚算完姿势时调用（在姿势 finalize 的 detour 里），把这一帧交给界面层写回滑条值。
    // 这条路径上不许调引擎接口、不许分配内存：只碰组件自己的骨骼数组。
    void OnPoseFinalized(void* component);

    // 把主角这一刻的移动速度交给搬运过来的 Locomotion 层。v1.3 原本由 Lua 每 150ms 送一次
    // loco 命令，我们没有 UE4SS 的 Lua，所以自己算：<30 待机 / <300 行走 / <700 奔跑 / 其余冲刺
    // （阈值取自 v1.3 的 config.lua）。
    void SetLocoSpeed(double centimeters_per_second);

    // 调整对象（武器档）：unarmed / weapon_1 / weapon_2 / weapon_3，命名沿用 v1.3 的 Lua 那套。
    void SetLocoContext(const char* context);

    // [NpcBody] Enabled (default 0): without it nothing of the hero's body is touched. Toggle turns it on.
    bool Enabled();
    void SetEnabled(bool on);
    // show: false starts the editor hidden (it applies the last session's sliders); F11 / the page's button shows it.
    void Start(bool show = true);
    void Stop();
    void Toggle();
    bool Visible();
    bool Started();

    // 预设（UUEPBS Presets 文件夹里的 .json，和编辑器面板同一份）：任意线程，不需要编辑器开着。Load 把预设换进现在的滑条
    // （骨骼、形变、移动状态层），Save 存下现在的滑条（同名覆盖）；message 是面板上会显示的那句话。
    std::vector<std::string> PresetNames();
    bool PresetLoad(const std::string& name, std::string& message);
    bool PresetSave(const std::string& name, std::string& message);
    bool PresetRemove(const std::string& name, std::string& message);
}
