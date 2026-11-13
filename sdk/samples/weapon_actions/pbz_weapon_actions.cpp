// pbz_weapon_actions — 示例派生插件：把主角某个具体的武器动作变成一个「插件信号」。
//
// 游戏里同一个键会按情况放出不同的动作（地面轻攻击、空中轻攻击、冲刺攻击……），派生链只看得到「轻攻击」这个键。
// 这个插件监视主角开始的每个技能（on_hero_ability），技能名符合规则时按一下对应的信号；派生链里用这个信号做
// 自定义按键起端、按键分支或武器派生修改的自定义按键行，就能只在「空中轻攻击」时接自己的派生。
// 派生链此刻正在等这个信号时（signal_wanted），还可以取消游戏原本的动作（consume），让派生链的招式替代它。
//
// 规则写在 PBZPlugins\pbz_weapon_actions.txt（UTF-8，每行一个信号，# 开头为注释）：
//     信号id | 显示名 | 技能名规则;规则2 | 取消原动作(1/0)
// 规则是技能名里包含的一段文字；以 = 开头表示技能名必须完全相同。没有这个文件时用下面 kDefaults 的三条。
// 主角每放出一种新技能，插件把它的名字写进 PBZDebugMenu.log 一次（"ability seen: ..."），照着写规则即可。
//
// 编译：上一层的 CMakeLists.txt，或任意 x64 C/C++ 编译器：cl /LD /O2 /EHsc /utf-8 pbz_weapon_actions.cpp /I ..\..\include
#include "pbz_combo_api.h"
#include <windows.h>
#include <fstream>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace {
const PbzApi* api = nullptr;
PbzPlugin* self = nullptr;

struct Rule { std::string id, name; std::vector<std::string> patterns; bool consume = false; };
std::vector<Rule> rules;
std::set<std::string> seen;  // 已写进日志的技能名

const char* const kDefaults[] = {
    "air_light | 空中轻攻击 | _Air_A | 1",
    "air_heavy | 空中重攻击 | _Air_B | 1",
    "dash_attack | 冲刺攻击 | _Sprint;_Dash_Attack | 0",
};

std::string Trim(const std::string& text) {
    const size_t a = text.find_first_not_of(" \t\r\n"), b = text.find_last_not_of(" \t\r\n");
    return a == std::string::npos ? std::string() : text.substr(a, b - a + 1);
}
std::vector<std::string> Split(const std::string& text, char sep) {
    std::vector<std::string> out; std::string part; std::istringstream in(text);
    while (std::getline(in, part, sep)) out.push_back(Trim(part));
    return out;
}
bool ParseRule(const std::string& line, Rule& rule) {
    const std::vector<std::string> f = Split(line, '|');
    if (f.size() < 3 || f[0].empty() || f[1].empty() || f[2].empty()) return false;
    rule = Rule{}; rule.id = f[0]; rule.name = f[1];
    for (const std::string& p : Split(f[2], ';')) if (!p.empty()) rule.patterns.push_back(p);
    rule.consume = f.size() > 3 && f[3] == "1";
    return !rule.patterns.empty();
}
// PBZPlugins\pbz_weapon_actions.txt next to this DLL.
std::wstring RulesPath() {
    HMODULE module = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, reinterpret_cast<LPCWSTR>(&RulesPath), &module);
    wchar_t path[MAX_PATH]{};
    const DWORD n = GetModuleFileNameW(module, path, MAX_PATH);
    std::wstring dir(path, n);
    const size_t slash = dir.find_last_of(L"\\/");
    return (slash == std::wstring::npos ? std::wstring() : dir.substr(0, slash + 1)) + L"pbz_weapon_actions.txt";
}
void LoadRules() {
    std::ifstream file(RulesPath());
    std::string line;
    bool any = false;
    while (file && std::getline(file, line)) {
        if (!line.empty() && static_cast<unsigned char>(line[0]) == 0xEF) line = line.substr(line.size() >= 3 ? 3 : line.size());  // BOM
        line = Trim(line);
        if (line.empty() || line[0] == '#') continue;
        Rule rule; if (ParseRule(line, rule)) { rules.push_back(rule); any = true; }
    }
    if (!any) for (const char* d : kDefaults) { Rule rule; if (ParseRule(d, rule)) rules.push_back(rule); }
}
bool Matches(const Rule& rule, const char* ability) {
    const std::string name = ability ? ability : "";
    for (const std::string& p : rule.patterns) {
        if (p[0] == '=') { if (name == p.substr(1)) return true; }
        else if (name.find(p) != std::string::npos) return true;
    }
    return false;
}

int32_t PBZ_CALL OnHeroAbility(void*, const PbzAbilityEvent* e) {
    if (e->chain_own) return PBZ_PASS;  // 派生链自己放的技能
    if (e->name && seen.size() < 512 && seen.insert(e->name).second) api->log(self, (std::string("ability seen: ") + e->name).c_str());
    int32_t answer = PBZ_PASS;
    for (const Rule& rule : rules) {
        if (!Matches(rule, e->name)) continue;
        // 派生链正在等这个信号时才取消原动作：其余时候游戏照常放它，信号也照样按一下（没人等就什么都不发生）。
        const bool wanted = api->signal_wanted(self, rule.id.c_str()) != 0;
        api->fire_signal(self, rule.id.c_str());
        if (wanted && rule.consume) answer = PBZ_CONSUME;
    }
    return answer;
}
}  // namespace

PBZ_PLUGIN_EXPORT PbzResult PBZ_CALL PbzPluginMain(const PbzApi* a, PbzPlugin* s) {
    if (!a || a->version < 1) return PBZ_E_VERSION;
    api = a; self = s;
    PbzPluginInfo info{};
    info.size = sizeof(info); info.id = "pbz.weapon_actions"; info.name = "武器动作信号（示例）"; info.version = "1.0.0"; info.author = "豪之刃";
    info.description = "把空中轻攻击等具体的武器动作变成可以在派生链里使用的信号"; info.min_api = 1;
    if (PbzResult r = api->describe(self, &info); r != PBZ_OK) return r;
    LoadRules();
    for (const Rule& rule : rules) {
        PbzSignalDesc d{};
        d.size = sizeof(d); d.id = rule.id.c_str(); d.name = rule.name.c_str(); d.group = "武器动作";
        d.description = rule.consume ? "派生链在等它时取消游戏原本的动作" : "不取消游戏原本的动作";
        int32_t code = 0;
        if (api->register_signal(self, &d, &code) != PBZ_OK) api->log(self, ("signal refused: " + rule.id).c_str());
    }
    PbzCallbacks callbacks{};
    callbacks.size = sizeof(callbacks); callbacks.on_hero_ability = OnHeroAbility;
    api->set_callbacks(self, &callbacks);
    api->log(self, ("weapon_actions ready: " + std::to_string(rules.size()) + " signal(s)").c_str());
    return PBZ_OK;
}
