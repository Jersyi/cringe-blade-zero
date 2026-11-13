// 身材 · 和 UUEPBS 一模一样的「部位」分组与骨骼名匹配。
//
// 原版不是「一根骨一个滑块」：「大腿」是一个组，组里是 thigh_l / thigh_r 再加上
// thigh_twistCor_01_l 这种辅助骨；组还带一个 Children（子骨怎么跟）：scale_too 让
// 子骨跟着变，keep_size 让子骨只跟着动、不变粗细。之前只改一根骨、子骨一起放大，
// 所以看起来像「整条腿变粗」而不是「大腿变粗」—— 差别就在这个 Children。
//
// 分组表照抄 UUEPBS 的 Scripts/BoneDictionary.json（26 组，见下），骨骼名清洗规则
// 照抄 src/core/rig_names.cpp，归属算法照抄 src/core/body_groups.cpp 的
// resolve_body_groups（一个骨只属于一个组，Chain 组只留最上面那根）。
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace NpcBodyRig
{
    // 子骨怎么跟着动（对应预设里的 children 字段）
    enum class Spread : uint8_t
    {
        Chain = 0,  // scale_too：子骨完全跟随（缩放、旋转、位移都传下去）
        Keep = 1,   // keep_size：子骨跟着动、但保持自己的粗细
        Solo = 2,   // this_bone_only：只有这根骨变形，子骨原地不动
    };

    // 一根骨的调整，全部按这根骨自己的局部轴算（X = 沿骨方向）
    struct BoneEdit
    {
        double axis[3]{1.0, 1.0, 1.0};   // 缩放
        double turn[3]{0.0, 0.0, 0.0};   // 旋转（度，按 X→Y→Z 顺序）
        double shift[3]{0.0, 0.0, 0.0};  // 位移（cm）
        Spread spread{Spread::Chain};

        bool has_scale() const;
        bool has_turn() const;
        bool has_shift() const;
        bool neutral() const;
        void clamp();
    };

    const char* SpreadToken(Spread spread);
    const wchar_t* SpreadLabel(Spread spread);
    bool SpreadFromChildren(const std::string& text, Spread& out);

    // 骨骼名清洗：MOT_Thigh_L -> thigh，CC_Base_L_Upperarm -> upperarm
    std::string Stem(const std::string& name, std::vector<std::string>* helper_words = nullptr);
    std::string FoldCase(const std::string& text);
    // '*' 任意、'#' 任意数字；其余按字面（不分大小写）
    bool GlobMatch(const std::string& pattern, const std::string& text);
    // 左右互换（thigh_l <-> thigh_r），认不出左右就返回空串
    std::string MirrorName(const std::string& name);

    struct Group
    {
        std::string title;               // "Thighs"
        std::string note;                // 给玩家看的一句说明
        std::vector<std::string> bones;  // 这套骨架上真实命中的骨名（可能为空）
        Spread spread{Spread::Chain};
    };

    // 把整套骨名按 BoneDictionary 归到各部位；没有命中任何部位的骨不出现。
    // parents 为空也能跑，只是 Chain 组不会做「只留最顶骨」的裁剪。
    std::vector<Group> Resolve(const std::vector<std::string>& names, const std::vector<int32_t>& parents);

    int32_t GroupCount();
}  // namespace NpcBodyRig
