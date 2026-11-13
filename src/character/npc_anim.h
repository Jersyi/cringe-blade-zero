// UUEPBS - 程序化时间轴动画（眨眼这类小动作）
//
// 一个「动画」是一组轨道（track）；每条轨道选一组骨骼，按顺序排「节点」（node）：
//   * Move  驱动节点：在 duration 秒内，把这组骨骼从「上一个节点的值」平滑/渐入/渐出地
//                       过渡到 target（旋转 / 位移 / 缩放都是相对骨骼自身局部轴的增量）。
//   * Delay 等待节点：固定或随机区间等待若干秒，然后继续后面的节点。
// 节点可以任意增删、上下移动、混合排序；轨道可以循环（眨眼就是「随机等待 → 闭 → 开 → 循环」）。
//
// 求值方式是「带状态的逐帧推进」（anim_track_advance）：随机延迟每次进节点时重新抽一次，
// 所以每一下眨眼的间隔都不一样。
//
// 动画存在预设文件夹下的 Animations\<名字>.json，和滑条预设、Locomotion 层完全分开。
#pragma once

#include "npc_body_rig.h"

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace NpcAnim
{
        using NpcBodyRig::BoneEdit;
        using NpcBodyRig::Spread;
    // 缓动曲线：节点内部从 0 到 1 的进度如何映射到实际插值比例。
    enum class Ease : uint8_t
    {
        Linear = 0, // 匀速
        Smooth = 1, // 平滑（smoothstep，两头慢中间快）
        In = 2,     // 渐入：开始慢，越来越快
        Out = 3,    // 渐出：开始快，越来越慢
        InOut = 4,  // 渐入渐出
    };
    inline constexpr int kEaseCount = 5;

    const char* ease_token(Ease ease); // 存文件用的英文 token
    bool ease_from_token(std::string_view token, Ease& out);
    double ease_curve(Ease ease, double t); // t 已夹在 0..1

    enum class AnimNodeKind : uint8_t
    {
        Move = 0,  // 驱动节点
        Delay = 1, // 等待节点
    };
    const char* node_kind_token(AnimNodeKind kind);
    bool node_kind_from_token(std::string_view token, AnimNodeKind& out);

    struct AnimNode
    {
        AnimNodeKind kind{AnimNodeKind::Move};

        // 驱动节点
        double duration{0.20};                 // 过渡时长（秒）
        Ease ease{Ease::Smooth};               // 缓动
        BoneEdit target{};                     // 目标增量：axis=缩放倍数，turn=角度，shift=厘米

        // 等待节点
        bool random{false};                    // 固定 / 随机区间
        double wait{1.0};                      // 固定等待秒数
        double wait_min{1.0};                  // 随机下限
        double wait_max{3.0};                  // 随机上限
    };

    // 一条轨道：一组骨骼 + 一串节点。
    struct AnimTrack
    {
        std::vector<std::string> bones{};     // 显示用的骨骼名（比较时大小写不敏感）
        Spread spread{Spread::Chain};         // 子骨骼如何跟随（和骨骼编辑器一致）
        bool loop{true};                      // 走完最后一个节点后是否回到第一个
        bool enabled{true};                   // 单独开关这条轨道
        std::vector<AnimNode> nodes{};
    };

    struct Animation
    {
        std::string name{};      // 文件名（不含扩展名）
        bool enabled{true};      // 整个动画是否在游戏里生效
        std::vector<AnimTrack> tracks{};
    };

    // 一条轨道的播放状态（逐帧推进用；随机延迟每次进入节点时重抽）。
    struct TrackRuntime
    {
        int node{-1};        // 当前节点下标，-1 = 还没开始
        double elapsed{0.0}; // 当前节点已经过去多少秒
        double wait{0.0};    // 当前等待节点实际抽到的秒数
        BoneEdit from{};     // 当前驱动节点的起点
        BoneEdit current{};  // 当前值（叠加到骨骼上）
        bool finished{false};
    };

    struct AnimRuntime
    {
        std::vector<TrackRuntime> tracks{};
    };

    // 推进一条轨道 dt 秒，返回它此刻应该叠加的值。dt 可以为 0（只读当前值）。
    BoneEdit anim_track_advance(const AnimTrack& track, TrackRuntime& runtime, double dt);

    // 一个循环的总秒数（随机等待按下限估），给 UI 画时间轴用。
    double anim_track_cycle(const AnimTrack& track);

    // 当前节点下标与节点内进度（0..1），给 UI 高亮用。
    void anim_track_progress(const AnimTrack& track, const TrackRuntime& runtime, int& node, double& progress);

    // 0..1 的随机数（每次调用重新抽）。
    double anim_random01();

    // ---- 文件（预设文件夹下的 Animations 子文件夹） -------------------------
    std::filesystem::path anim_folder(const std::filesystem::path& preset_folder);
    std::vector<std::string> anim_list(const std::filesystem::path& preset_folder); // 名字，按字母序
    bool anim_load(const std::filesystem::path& preset_folder, const std::string& name, Animation& out, std::string& message);
    bool anim_save(const std::filesystem::path& preset_folder, const Animation& animation, std::string& message);
    bool anim_remove(const std::filesystem::path& preset_folder, const std::string& name, std::string& message);
    // 启动时一次性读全部动画（解析失败的会写进 log）。
    std::vector<Animation> anim_load_all(const std::filesystem::path& preset_folder, std::vector<std::string>* errors = nullptr);

    // 文件内容 <-> Animation（测试和手动编辑都用得着）
    std::string anim_to_json(const Animation& animation);
    bool anim_from_json(const std::string& text, Animation& out, std::string& message);
} // namespace NpcAnim
