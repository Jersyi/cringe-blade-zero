#pragma once
#include <iterator>
#include <string>
#include <string_view>

// Grab moves (MotianGrab::Variant::boss_id) and NPC skins (kProfiles ids) name one boss in different ways:
// GuiChai_Red / GuiChaiRedP2, YuDing_00 / YuDing, MBossVariants / MBossP2, DaTouWaWa / WuWuWu, MoTian / MoTianMask.
// Key folds a name to one key per boss: lower case without '_', then without a phase suffix (P2) and trailing
// digits, then the aliases below (a look of the same boss, or a skin named after the person, not the blueprint).
namespace GrabBoss {
struct Alias { const char* from; const char* to; };
constexpr Alias kAliases[] = {
    {"motianyoung", "motian"}, {"motianmask", "motian"},
    {"zuoshanglongti", "zuoshang"}, {"zuoshangyoung", "zuoshang"}, {"zuoshangbase", "zuoshang"},
    {"huashanblue", "huashan"}, {"huashanjianou", "huashan"},
    {"mbossvariants", "mboss"}, {"xukonghuanxiang", "xukong"},
    {"wuwuwu", "datouwawa"}, {"cangang", "mobmanelite"},
    {"guichaizhanqi", "guichaiyellow"}, {"guichaigouhun", "guichaiyellow"}, {"guichaiduanchang", "guichaicyan"},
    {"huodaoduizhang", "greatblademan"},
};
inline std::string Key(std::string_view id) {
    std::string key;
    for (const char c : id) if (c != '_') key += static_cast<char>(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c);
    if (key.size() > 2 && key.compare(key.size() - 2, 2, "p2") == 0) key.resize(key.size() - 2);
    while (!key.empty() && key.back() >= '0' && key.back() <= '9') key.pop_back();
    for (const Alias& a : kAliases) if (key == a.from) return a.to;
    return key;
}
inline std::string Key(std::wstring_view id) {
    std::string narrow;
    for (const wchar_t c : id) narrow += c < 128 ? static_cast<char>(c) : '?';
    return Key(std::string_view(narrow));
}
// True when both name the same boss (never for an empty name).
template <class A, class B> bool Same(const A& a, const B& b) {
    const std::string ka = Key(a), kb = Key(b);
    return !ka.empty() && ka == kb;
}
}
