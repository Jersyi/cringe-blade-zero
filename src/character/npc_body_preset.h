// 身材预设："<game>/platform/steam/UUEPBS Presets/<name>.json"
//
// 文件格式照抄 UUEPBS（见 docs/window_presets.png 那一页）：这样别人做好的预设能直接导入，
// 我这里存出来的也能给原版用。
//
//   {
//     "format": "UUEPBS preset",
//     "version": 2,
//     "bones": {
//       "thigh_l": { "length": 1.0, "width": 1.12, "depth": 1.12, "children": "keep_size" }
//     }
//   }
//
// length = 沿骨方向（X），width = Y，depth = Z；1 = 不变。
// rotate = 度（绕骨骼自己的 X、Y、Z，按这个顺序）；move = cm（X、Y、Z）；两个都可省。
// children 取值 scale_too | keep_size | this_bone_only。
#pragma once

#include "npc_body_rig.h"

#include <map>
#include <string>
#include <vector>

namespace NpcBodyPreset
{
    // 键是小写骨名（和原版一样按大小写无关匹配）。
    using Book = std::map<std::string, NpcBodyRig::BoneEdit>;

    // "<dll 所在目录>\UUEPBS Presets"，和原版落在同一个地方。
    const std::wstring& Folder();
    bool Ready();

    // 现有预设名（不含 _last_session），大小写无关字典序。
    std::vector<std::string> List();
    bool Exists(const std::string& name);

    // 名字里的 \ / : * ? " < > | 会被换成 _；结果为空 = 名字不能用。
    std::string CleanName(const std::string& raw);

    bool Save(const std::string& name, const Book& book, std::string& message);
    bool Load(const std::string& name, Book& out, std::string& message);
    bool Remove(const std::string& name, std::string& message);

    std::string Serialize(const Book& book);
    // 接受 JSON；顺带认旧版逐行的 .rbs 文本（按开头是不是 '{' 判断）。
    bool Parse(const std::string& text, Book& out, std::string& message);
} // namespace NpcBodyPreset
