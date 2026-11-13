// 身材 · 部位分组与骨骼名匹配的实现。
//
// 三块逻辑分别照抄 UUEPBS 的：
//   * src/core/rig_names.cpp   —— bone_stem / find_side / mirror_bone_name / glob_match
//   * Scripts/BoneDictionary.json —— 下面的 kGroups 表（26 组）
//   * src/core/body_groups.cpp  —— Resolve() 的归属顺序、Extra、Helpers、Chain 只留最顶骨
//
// 为什么非要用它这套：玩家的预设文件里写的是骨骼原名（thigh_twistCor_01_l 这种），
// 只有照它的清洗与匹配规则走，别人的预设才能原封不动导进来。
#include "npc_body_rig.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <string_view>

namespace NpcBodyRig
{
    namespace
    {
        constexpr double kMinScale = 0.02;
        constexpr double kMaxScale = 20.0;
        constexpr double kMaxTurn = 180.0;
        constexpr double kMaxShift = 200.0;
        constexpr double kNeutralEps = 1.0e-6;

        bool IsSep(char c)
        {
            return c == '_' || c == '.' || c == '-' || c == ' ' || c == ':' || c == '|';
        }
        bool IsUpper(char c) { return c >= 'A' && c <= 'Z'; }
        bool IsLower(char c) { return c >= 'a' && c <= 'z'; }
        bool IsAlpha(char c) { return IsUpper(c) || IsLower(c); }
        bool IsDigit(char c) { return c >= '0' && c <= '9'; }
        char Lower(char c) { return static_cast<char>(std::tolower(static_cast<unsigned char>(c))); }

        bool StartsWithCi(std::string_view s, std::string_view p)
        {
            if (p.empty() || s.size() <= p.size())
            {
                return false;
            }
            for (size_t i = 0; i < p.size(); ++i)
            {
                if (Lower(s[i]) != Lower(p[i]))
                {
                    return false;
                }
            }
            return true;
        }
        bool EndsWithCi(std::string_view s, std::string_view p)
        {
            if (p.empty() || s.size() <= p.size())
            {
                return false;
            }
            const size_t off = s.size() - p.size();
            for (size_t i = 0; i < p.size(); ++i)
            {
                if (Lower(s[off + i]) != Lower(p[i]))
                {
                    return false;
                }
            }
            return true;
        }
        bool StartsWith(std::string_view s, std::string_view p)
        {
            return s.size() >= p.size() && s.compare(0, p.size(), p) == 0;
        }
        bool EndsWith(std::string_view s, std::string_view p)
        {
            return s.size() >= p.size() && s.compare(s.size() - p.size(), p.size(), p) == 0;
        }

        enum class Side
        {
            None,
            Left,
            Right
        };
        struct SideMark
        {
            Side side = Side::None;
            size_t pos = 0;
            size_t len = 0;
        };

        bool WordAt(std::string_view s, size_t pos, std::string_view word)
        {
            if (pos + word.size() > s.size())
            {
                return false;
            }
            for (size_t i = 0; i < word.size(); ++i)
            {
                if (Lower(s[pos + i]) != word[i])
                {
                    return false;
                }
            }
            return true;
        }

        // "Left"/"Right" 作为整词或驼峰的一部分：LeftArm、hand_left、Left Hand。
        SideMark FindWord(std::string_view s)
        {
            SideMark best;
            const std::pair<std::string_view, Side> words[2] = {{"left", Side::Left}, {"right", Side::Right}};
            for (const auto& entry : words)
            {
                const std::string_view word = entry.first;
                for (size_t pos = 0; pos + word.size() <= s.size(); ++pos)
                {
                    if (!WordAt(s, pos, word))
                    {
                        continue;
                    }
                    const char first = s[pos];
                    const bool start_ok = pos == 0 || !IsAlpha(s[pos - 1]) || (IsUpper(first) && IsLower(s[pos - 1]));
                    const size_t after = pos + word.size();
                    const bool whole_upper = after < s.size() && IsUpper(first) && IsUpper(s[after]);
                    bool end_ok = after == s.size() || !IsAlpha(s[after]) || (IsUpper(s[after]) && !whole_upper);
                    if (whole_upper && after < s.size() && IsUpper(s[after]))
                    {
                        end_ok = false;  // "LEFTARM" 分不清左右
                    }
                    if (start_ok && end_ok && (best.side == Side::None || pos > best.pos))
                    {
                        best = {entry.second, pos, word.size()};
                    }
                }
            }
            return best;
        }

        // 分隔符之间的孤立 l/r：thigh_l、thigh.L、J_Bip_L_Arm、L_Hand、horn1_l_001。
        SideMark FindLetter(std::string_view s)
        {
            SideMark best;
            for (size_t pos = 0; pos < s.size(); ++pos)
            {
                const char c = Lower(s[pos]);
                if (c != 'l' && c != 'r')
                {
                    continue;
                }
                const bool start_ok = pos == 0 || IsSep(s[pos - 1]);
                bool end_ok = pos + 1 == s.size() || IsSep(s[pos + 1]);
                if (!end_ok && pos > 0 && IsSep(s[pos - 1]))
                {
                    size_t k = pos + 1;
                    while (k < s.size() && IsDigit(s[k]))
                    {
                        ++k;
                    }
                    end_ok = k > pos + 1 && k == s.size();  // Breast_jnt_L01
                }
                const bool has_sep = (pos > 0 && IsSep(s[pos - 1])) || (pos + 1 < s.size() && IsSep(s[pos + 1]));
                if (start_ok && end_ok && has_sep && s.size() > 2)
                {
                    best = {c == 'l' ? Side::Left : Side::Right, pos, 1};
                }
            }
            return best;
        }

        // Daz Genesis：lThighBend、rShldrTwist（可带命名空间前缀）。
        SideMark FindDaz(std::string_view s)
        {
            size_t start = s.rfind(':');
            start = start == std::string_view::npos ? 0 : start + 1;
            if (start + 2 <= s.size() && (s[start] == 'l' || s[start] == 'r') && IsUpper(s[start + 1]))
            {
                return {s[start] == 'l' ? Side::Left : Side::Right, start, 1};
            }
            return {};
        }

        SideMark FindSide(std::string_view name)
        {
            const SideMark word = FindWord(name);
            const SideMark letter = FindLetter(name);
            if (word.side != Side::None && letter.side != Side::None)
            {
                return letter.pos > word.pos ? letter : word;
            }
            if (word.side != Side::None)
            {
                return word;
            }
            if (letter.side != Side::None)
            {
                return letter;
            }
            return FindDaz(name);
        }

        std::string SwapWord(std::string_view original, Side from)
        {
            const std::string_view to = from == Side::Left ? "right" : "left";
            std::string out(to);
            const bool all_upper = original.size() > 1 && IsUpper(original[0]) && IsUpper(original[1]);
            for (size_t i = 0; i < out.size(); ++i)
            {
                if (all_upper || (i == 0 && IsUpper(original[0])))
                {
                    out[i] = static_cast<char>(std::toupper(static_cast<unsigned char>(out[i])));
                }
            }
            return out;
        }

        struct StemRules
        {
            const char* const* raw_prefixes;
            int raw_prefix_count;
            const char* const* raw_suffixes;
            int raw_suffix_count;
            const char* const* stem_prefixes;
            int stem_prefix_count;
        };

        const char* const kRawPrefixes[] = {"MOT_", "SUP_", "PRG_", "SCB_", "SBC_", "CC_Base_", "J_Bip_C_", "J_Bip_", "J_Sec_", "J_Adj_",
                                            "Bip001 ", "Bip01 ", "Bip001_", "Bip01_", "DEF-", "ORG-", "MCH-", "ValveBiped."};
        const char* const kRawSuffixes[] = {"_M", ".M", "_C", ".C", "_x", ".x", "_SkinJNT", "_jnt", "_bind"};
        const char* const kStemPrefixes[] = {"mixamorig", "ccbase", "jbipc", "jbip", "jsec", "jadj", "bip001", "bip01", "def", "org", "valvebiped"};

        const StemRules& Rules()
        {
            static const StemRules rules = {
                kRawPrefixes, static_cast<int>(sizeof(kRawPrefixes) / sizeof(kRawPrefixes[0])),
                kRawSuffixes, static_cast<int>(sizeof(kRawSuffixes) / sizeof(kRawSuffixes[0])),
                kStemPrefixes, static_cast<int>(sizeof(kStemPrefixes) / sizeof(kStemPrefixes[0])),
            };
            return rules;
        }

        // 组表：照抄 BoneDictionary.json 的 Bones / Extra / Helpers / MatchAll / Children。
        struct GroupDef
        {
            const char* title;
            const char* note;
            const char* const* bones;
            int bone_count;
            const char* const* extra;  // 可空
            int extra_count;
            bool helpers;
            bool match_all;
            Spread spread;
        };

        const char* const kHead[] = {"head", "head#"};
        const char* const kNeck[] = {"neck01", "neck", "necklower", "neck1", "neck#"};
        const char* const kHorns[] = {"horn1", "horn", "horn#"};
        const char* const kEars[] = {"cear01", "ear", "ear#"};
        const char* const kEyes[] = {"ceye", "eye", "eyeball", "eye#"};
        const char* const kRibCage[] = {"spine02", "spine2", "chestlower", "chest", "abdomenupper", "spine002", "ribs", "ribcage", "*xiong"};
        const char* const kWaist[] = {"spine01", "abdomenlower", "abdomen", "spine", "spine001", "waist", "spine1", "*yaobu*", "*koshi*"};
        const char* const kBelly[] = {"belly", "belly#", "stomach", "tummy", "*belly*", "*fubu*"};
        const char* const kHips[] = {"pelvis", "hips", "hip", "pelvis#"};
        const char* const kShoulders[] = {"clavicle", "shoulder", "collar", "clavicle#", "scapula"};
        const char* const kBreasts[] = {"boob",       "breast",    "bust1",     "bust",     "pectoral",   "breastbase", "chestbreast",
                                        "*breast",    "breast*",   "boob*",     "bust*",    "*oppai*",    "*rufang*",   "*breast*"};
        const char* const kNipples[] = {"nipp", "nipple", "nipple#", "*nipple*", "nipp#"};
        const char* const kButt[] = {"butt",    "glute",   "buttock", "gluteus", "butt#",   "buttcheek",
                                     "*butt*",  "*glute*", "*buttock*", "*pigu*", "*shiri*", "*tunbu*"};
        const char* const kThighs[] = {"thigh", "upleg", "upperleg", "thighbend", "thigh#", "thighstretch", "*datui*", "*momo*"};
        const char* const kThighsExtra[] = {"*dtui*", "thighjiggle#"};
        const char* const kCalves[] = {"calf", "lowerleg", "leg", "shin", "calf#", "legstretch", "knee", "*xiaotui*"};
        const char* const kCalvesExtra[] = {"*xtui*"};
        const char* const kFeet[] = {"foot", "foot#", "ankle"};
        const char* const kUpperArms[] = {"upperarm", "arm", "shldrbend", "upperarm#", "armstretch", "*dabi*"};
        const char* const kForearms[] = {"lowerarm", "forearm", "forearmbend", "lowerarm#", "forearmstretch", "elbow", "*xiaobi*"};
        const char* const kHands[] = {"hand", "hand#", "wrist"};
        const char* const kDeltoids[] = {"*deltoid*", "delt", "delt#", "*shoulderdelt*"};
        const char* const kBiceps[] = {"*bicep*"};
        const char* const kTriceps[] = {"*tricep*"};
        const char* const kPecs[] = {"*pectoral*", "pec", "pec#", "pecs", "*pecs*", "*claviclepec*", "*chestmuscle*"};
        const char* const kLats[] = {"*latissimus*", "lat", "lats", "lat#", "*lats"};
        const char* const kTraps[] = {"*trapezius*", "trap", "traps", "trap#"};
        const char* const kSerratus[] = {"*serratus*"};
        const char* const kAbs[] = {"abs", "abs#", "*abdominis*", "*sixpack*"};
        const char* const kObliques[] = {"*oblique*"};
        const char* const kQuads[] = {"*quadricep*", "*quads*", "quad", "quad#"};
        const char* const kHamstrings[] = {"*hamstring*"};
        const char* const kCalfMuscles[] = {"*gastroc*", "*soleus*"};

#define NPC_RIG_GROUP(title, note, array, extra_array, helpers, all, spread)                                       \
    {                                                                                                              \
        title, note, array, static_cast<int>(sizeof(array) / sizeof(array[0])),                                    \
            (extra_array), static_cast<int>(sizeof(extra_array) / sizeof(extra_array[0])), (helpers), (all), (spread) \
    }
#define NPC_RIG_GROUP_NX(title, note, array, helpers, all, spread) \
    {title, note, array, static_cast<int>(sizeof(array) / sizeof(array[0])), nullptr, 0, (helpers), (all), (spread)}

        const GroupDef kGroups[] = {
            NPC_RIG_GROUP_NX("Head", "头的大小", kHead, false, false, Spread::Chain),
            NPC_RIG_GROUP_NX("Neck", "脖子的长短粗细", kNeck, true, false, Spread::Keep),
            NPC_RIG_GROUP_NX("Horns", "角", kHorns, false, false, Spread::Chain),
            NPC_RIG_GROUP_NX("Ears", "耳朵", kEars, false, false, Spread::Chain),
            NPC_RIG_GROUP_NX("Eyes", "眼睛", kEyes, false, false, Spread::Chain),
            NPC_RIG_GROUP_NX("Rib cage", "胸腔 / 上身", kRibCage, false, false, Spread::Keep),
            NPC_RIG_GROUP_NX("Waist", "腰的粗细", kWaist, false, false, Spread::Keep),
            NPC_RIG_GROUP_NX("Belly", "小腹", kBelly, false, false, Spread::Chain),
            NPC_RIG_GROUP_NX("Hips", "胯宽", kHips, false, false, Spread::Keep),
            NPC_RIG_GROUP_NX("Shoulders", "肩宽", kShoulders, false, false, Spread::Keep),
            NPC_RIG_GROUP_NX("Breasts", "胸围", kBreasts, false, false, Spread::Chain),
            NPC_RIG_GROUP_NX("Nipples", "乳首", kNipples, false, false, Spread::Chain),
            NPC_RIG_GROUP_NX("Butt", "臀围", kButt, false, false, Spread::Chain),
            NPC_RIG_GROUP("Thighs", "大腿的粗细", kThighs, kThighsExtra, true, false, Spread::Keep),
            NPC_RIG_GROUP("Calves", "小腿的粗细", kCalves, kCalvesExtra, true, false, Spread::Keep),
            NPC_RIG_GROUP_NX("Feet", "脚的大小", kFeet, false, false, Spread::Chain),
            NPC_RIG_GROUP_NX("Upper arms", "上臂的粗细", kUpperArms, true, false, Spread::Keep),
            NPC_RIG_GROUP_NX("Forearms", "小臂的粗细", kForearms, true, false, Spread::Keep),
            NPC_RIG_GROUP_NX("Hands", "手的大小", kHands, false, false, Spread::Chain),
            NPC_RIG_GROUP_NX("Deltoids", "三角肌", kDeltoids, false, true, Spread::Chain),
            NPC_RIG_GROUP_NX("Biceps", "二头肌", kBiceps, false, true, Spread::Chain),
            NPC_RIG_GROUP_NX("Triceps", "三头肌", kTriceps, false, true, Spread::Chain),
            NPC_RIG_GROUP_NX("Pecs", "胸肌", kPecs, false, true, Spread::Chain),
            NPC_RIG_GROUP_NX("Lats", "背阔肌", kLats, false, true, Spread::Chain),
            NPC_RIG_GROUP_NX("Traps", "斜方肌", kTraps, false, true, Spread::Chain),
            NPC_RIG_GROUP_NX("Serratus", "前锯肌", kSerratus, false, true, Spread::Chain),
            NPC_RIG_GROUP_NX("Abs", "腹肌", kAbs, false, true, Spread::Chain),
            NPC_RIG_GROUP_NX("Obliques", "腹斜肌", kObliques, false, true, Spread::Chain),
            NPC_RIG_GROUP_NX("Quads", "股四头肌", kQuads, false, true, Spread::Chain),
            NPC_RIG_GROUP_NX("Hamstrings", "腿后肌", kHamstrings, false, true, Spread::Chain),
            NPC_RIG_GROUP_NX("Calf muscles", "小腿肌", kCalfMuscles, false, true, Spread::Chain),
        };
#undef NPC_RIG_GROUP
#undef NPC_RIG_GROUP_NX

        const char* const kHelperWords[] = {"twist", "roll", "jiggle", "bend", "share", "helper", "stretch", "part", "correct", "thicker", "volume", "fix"};

        struct Bone
        {
            std::string name;
            std::string stem;
        };

        // 以 '=' 开头匹配骨骼原名，否则匹配清洗后的 stem。
        bool Matches(const std::string& pattern, const Bone& bone)
        {
            if (!pattern.empty() && pattern[0] == '=')
            {
                return GlobMatch(pattern.substr(1), bone.name);
            }
            return GlobMatch(pattern, bone.stem);
        }

        bool HasAncestorIn(int32_t i, const std::vector<int32_t>& parents, const std::vector<uint8_t>& in_group)
        {
            for (int32_t p = parents[static_cast<size_t>(i)]; p >= 0; p = parents[static_cast<size_t>(p)])
            {
                if (in_group[static_cast<size_t>(p)])
                {
                    return true;
                }
            }
            return false;
        }

        bool Contains(const std::vector<size_t>& list, size_t value)
        {
            return std::find(list.begin(), list.end(), value) != list.end();
        }

        const char* const* ArrayPtr(const char* const* p) { return p; }
    }  // namespace

    bool BoneEdit::has_scale() const
    {
        return std::fabs(axis[0] - 1.0) >= kNeutralEps || std::fabs(axis[1] - 1.0) >= kNeutralEps || std::fabs(axis[2] - 1.0) >= kNeutralEps;
    }
    bool BoneEdit::has_turn() const
    {
        return std::fabs(turn[0]) >= kNeutralEps || std::fabs(turn[1]) >= kNeutralEps || std::fabs(turn[2]) >= kNeutralEps;
    }
    bool BoneEdit::has_shift() const
    {
        return std::fabs(shift[0]) >= kNeutralEps || std::fabs(shift[1]) >= kNeutralEps || std::fabs(shift[2]) >= kNeutralEps;
    }
    bool BoneEdit::neutral() const { return !has_scale() && !has_turn() && !has_shift(); }

    void BoneEdit::clamp()
    {
        for (int a = 0; a < 3; ++a)
        {
            if (!std::isfinite(axis[a]))
            {
                axis[a] = 1.0;
            }
            axis[a] = (std::min)(kMaxScale, (std::max)(kMinScale, axis[a]));
            if (!std::isfinite(turn[a]))
            {
                turn[a] = 0.0;
            }
            turn[a] = (std::min)(kMaxTurn, (std::max)(-kMaxTurn, turn[a]));
            if (!std::isfinite(shift[a]))
            {
                shift[a] = 0.0;
            }
            shift[a] = (std::min)(kMaxShift, (std::max)(-kMaxShift, shift[a]));
        }
        if (static_cast<uint8_t>(spread) > static_cast<uint8_t>(Spread::Solo))
        {
            spread = Spread::Chain;
        }
    }

    const char* SpreadToken(Spread spread)
    {
        switch (spread)
        {
        case Spread::Chain:
            return "scale_too";
        case Spread::Keep:
            return "keep_size";
        case Spread::Solo:
            return "this_bone_only";
        }
        return "scale_too";
    }

    const wchar_t* SpreadLabel(Spread spread)
    {
        switch (spread)
        {
        case Spread::Chain:
            return L"子骨跟着变";
        case Spread::Keep:
            return L"子骨保持粗细";
        case Spread::Solo:
            return L"只这一根";
        }
        return L"子骨跟着变";
    }

    bool SpreadFromChildren(const std::string& text, Spread& out)
    {
        const std::string t = FoldCase(text);
        if (t == "scale_too" || t == "chain" || t == "full" || t == "hierarchy")
        {
            out = Spread::Chain;
            return true;
        }
        if (t == "keep_size" || t == "keep" || t == "preserve")
        {
            out = Spread::Keep;
            return true;
        }
        if (t == "this_bone_only" || t == "solo" || t == "isolated" || t == "bone")
        {
            out = Spread::Solo;
            return true;
        }
        return false;
    }

    std::string FoldCase(const std::string& text)
    {
        std::string out(text);
        for (char& c : out)
        {
            c = Lower(c);
        }
        return out;
    }

    std::string MirrorName(const std::string& name)
    {
        const SideMark mark = FindSide(name);
        if (mark.side == Side::None)
        {
            return std::string();
        }
        std::string out(name.substr(0, mark.pos));
        if (mark.len == 1)
        {
            const char c = name[mark.pos];
            const bool upper = IsUpper(c);
            const char swapped = mark.side == Side::Left ? 'r' : 'l';
            out += upper ? static_cast<char>(std::toupper(swapped)) : swapped;
        }
        else
        {
            out += SwapWord(std::string_view(name).substr(mark.pos, mark.len), mark.side);
        }
        out += name.substr(mark.pos + mark.len);
        return out;
    }

    std::string Stem(const std::string& name, std::vector<std::string>* helper_words)
    {
        if (helper_words)
        {
            helper_words->assign(std::begin(kHelperWords), std::end(kHelperWords));
        }
        const StemRules& r = Rules();
        std::string raw(name);
        const size_t ns = raw.find_last_of(":|");
        if (ns != std::string::npos)
        {
            raw.erase(0, ns + 1);
        }
        for (bool again = true; again;)
        {
            again = false;
            for (int i = 0; i < r.raw_prefix_count; ++i)
            {
                if (StartsWithCi(raw, r.raw_prefixes[i]))
                {
                    raw.erase(0, std::strlen(r.raw_prefixes[i]));
                    again = true;
                }
            }
        }
        const SideMark mark = FindSide(raw);
        if (mark.side != Side::None)
        {
            raw.erase(mark.pos, mark.len);
        }
        for (bool again = true; again;)
        {
            again = false;
            while (!raw.empty() && (raw.back() == '_' || raw.back() == '.' || raw.back() == '-' || raw.back() == ' '))
            {
                raw.pop_back();
            }
            for (int i = 0; i < r.raw_suffix_count; ++i)
            {
                if (EndsWithCi(raw, r.raw_suffixes[i]))
                {
                    raw.erase(raw.size() - std::strlen(r.raw_suffixes[i]));
                    again = true;
                }
            }
        }
        std::string s;
        for (const char c : raw)
        {
            if (std::isalnum(static_cast<unsigned char>(c)))
            {
                s += Lower(c);
            }
        }
        for (bool again = true; again;)
        {
            again = false;
            for (int i = 0; i < r.stem_prefix_count; ++i)
            {
                const std::string_view p = r.stem_prefixes[i];
                if (s.size() > p.size() && s.compare(0, p.size(), p) == 0)
                {
                    s.erase(0, p.size());
                    again = true;
                }
            }
        }
        return s;
    }

    bool GlobMatch(const std::string& pattern, const std::string& text)
    {
        size_t p = 0, t = 0, star_p = std::string::npos, star_t = 0;
        while (t < text.size())
        {
            if (p < pattern.size() && pattern[p] == '#')
            {
                size_t k = t;
                while (k < text.size() && IsDigit(text[k]))
                {
                    ++k;
                }
                t = k;
                ++p;
                continue;
            }
            if (p < pattern.size() && pattern[p] == '*')
            {
                star_p = p++;
                star_t = t;
                continue;
            }
            if (p < pattern.size() && Lower(pattern[p]) == Lower(text[t]))
            {
                ++p;
                ++t;
                continue;
            }
            if (star_p != std::string::npos)
            {
                p = star_p + 1;
                t = ++star_t;
                continue;
            }
            return false;
        }
        while (p < pattern.size() && (pattern[p] == '*' || pattern[p] == '#'))
        {
            ++p;
        }
        return p == pattern.size();
    }

    int32_t GroupCount() { return static_cast<int32_t>(sizeof(kGroups) / sizeof(kGroups[0])); }

    std::vector<Group> Resolve(const std::vector<std::string>& names, const std::vector<int32_t>& parents)
    {
        std::vector<Group> out;
        const size_t n = names.size();
        if (n == 0)
        {
            return out;
        }
        const bool have_parents = parents.size() == n;

        std::vector<Bone> bones(n);
        for (size_t i = 0; i < n; ++i)
        {
            bones[i].name = names[i];
            bones[i].stem = Stem(names[i]);
        }

        // 一个骨最多属于一个组；按组表顺序分配，先到先得（和原版一致）。
        std::vector<uint8_t> taken(n, 0);
        for (int32_t gi = 0; gi < GroupCount(); ++gi)
        {
            const GroupDef& def = kGroups[gi];
            std::vector<size_t> picked;
            std::string chosen_stem;
            for (int32_t bi = 0; bi < def.bone_count; ++bi)
            {
                const std::string pattern = def.bones[bi];
                bool hit = false;
                for (size_t i = 0; i < n; ++i)
                {
                    if (!taken[i] && Matches(pattern, bones[i]) && !Contains(picked, i))
                    {
                        picked.push_back(i);
                        hit = true;
                        if (chosen_stem.empty())
                        {
                            chosen_stem = bones[i].stem;
                        }
                    }
                }
                if (hit && !def.match_all)
                {
                    break;  // 第一个命中的 pattern 就停
                }
            }
            if (picked.empty())
            {
                continue;
            }
            for (int32_t ei = 0; ei < def.extra_count; ++ei)
            {
                const std::string pattern = def.extra[ei];
                for (size_t i = 0; i < n; ++i)
                {
                    if (!taken[i] && Matches(pattern, bones[i]) && !Contains(picked, i))
                    {
                        picked.push_back(i);
                    }
                }
            }
            // 辅助骨：thigh_twistCor_01_l 这种跟在主骨后面的扭转骨 / 拉伸骨。
            if (def.helpers && !chosen_stem.empty())
            {
                std::string base = chosen_stem;
                if (base.size() > 4 && EndsWith(base, "bend"))
                {
                    base.resize(base.size() - 4);
                }
                if (base.size() > 7 && EndsWith(base, "stretch"))
                {
                    base.resize(base.size() - 7);
                }
                while (!base.empty() && IsDigit(base.back()))
                {
                    base.pop_back();
                }
                for (size_t i = 0; i < n && !base.empty(); ++i)
                {
                    const std::string& s = bones[i].stem;
                    if (taken[i] || s.size() <= base.size() || s.compare(0, base.size(), base) != 0 || Contains(picked, i))
                    {
                        continue;
                    }
                    const std::string_view rest(s.c_str() + base.size());
                    for (const char* word : kHelperWords)
                    {
                        if (StartsWith(rest, word))
                        {
                            picked.push_back(i);
                            break;
                        }
                    }
                }
            }
            // 「子骨跟着变」本来就会把缩放传下去，所以只留最上面那一根，
            // 否则一根链上的四根骨会被放大四次。
            if (def.spread == Spread::Chain && have_parents)
            {
                std::vector<uint8_t> in_group(n, 0);
                for (const size_t i : picked)
                {
                    in_group[i] = 1;
                }
                picked.erase(std::remove_if(picked.begin(), picked.end(),
                                            [&](size_t i) { return HasAncestorIn(static_cast<int32_t>(i), parents, in_group); }),
                             picked.end());
            }
            if (picked.empty())
            {
                continue;
            }
            std::sort(picked.begin(), picked.end());
            Group group;
            group.title = def.title;
            group.note = def.note;
            group.spread = def.spread;
            for (const size_t i : picked)
            {
                taken[i] = 1;
                group.bones.push_back(names[i]);
            }
            out.push_back(std::move(group));
        }
        return out;
    }
}  // namespace NpcBodyRig
