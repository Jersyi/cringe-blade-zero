// pbz_conditions — 示例派生插件：几个「判断」节点和「动作」节点。
//
// 判断节点（PBZ_ACTION_DECIDE）：按键经过时按主角 / 目标的情况选出口。
//   主角血量   出口：低于 / 不低于           数值：主角血量低于 N %
//   目标血量   出口：低于 / 不低于 / 没有目标  数值：目标血量低于 N %
//   目标距离   出口：在距离内 / 更远 / 没有目标 数值：距离 N 厘米
//   空中       出口：空中 / 地面
//   概率       出口：命中 / 未命中           数值：命中概率 N %
// 动作节点（PBZ_ACTION_RUN）：
//   提示       在画面右下角显示「插件节点：数值 N」
//   结束派生链 结束当前派生链
//
// 编译：上一层的 CMakeLists.txt，或任意 x64 C/C++ 编译器：cl /LD /O2 /utf-8 pbz_conditions.cpp /I ..\..\include
#include "pbz_combo_api.h"
#include <stdio.h>

namespace {
const PbzApi* api = nullptr;
PbzPlugin* self = nullptr;

// 主角和目标的情况（get_hero 每帧最多读一次，这里每次判断都可以放心调用）。
bool Hero(PbzHeroFacts& facts) {
    facts = PbzHeroFacts{};
    facts.size = sizeof(facts);
    return api->get_hero(&facts) == PBZ_OK && facts.valid;
}
int32_t PBZ_CALL HeroHp(void*, const PbzNodeCall* call) {
    PbzHeroFacts h;
    if (!Hero(h) || h.health_max <= 0) return -1;  // 读不到：不走任何出口（按键像没有接上一样）
    return h.health * 100.0f < static_cast<float>(call->value) * h.health_max ? 0 : 1;
}
int32_t PBZ_CALL TargetHp(void*, const PbzNodeCall* call) {
    PbzHeroFacts h;
    if (!Hero(h) || !h.has_target || h.target_health_max <= 0) return 2;
    return h.target_health * 100.0f < static_cast<float>(call->value) * h.target_health_max ? 0 : 1;
}
int32_t PBZ_CALL TargetDistance(void*, const PbzNodeCall* call) {
    PbzHeroFacts h;
    if (!Hero(h) || !h.has_target) return 2;
    return h.target_distance <= static_cast<float>(call->value) ? 0 : 1;
}
int32_t PBZ_CALL Airborne(void*, const PbzNodeCall*) {
    PbzHeroFacts h;
    if (!Hero(h)) return -1;
    return h.airborne ? 0 : 1;
}
// 同一次按键在同一帧里只问一次（MOD 会缓存结果），所以随机数不会前后不一致。
uint32_t seed = 0;
int32_t PBZ_CALL Chance(void*, const PbzNodeCall* call) {
    if (!seed) seed = static_cast<uint32_t>(call->now_ms) | 1u;
    seed ^= seed << 13; seed ^= seed >> 17; seed ^= seed << 5;
    return static_cast<int32_t>(seed % 100u) < call->value ? 0 : 1;
}
void PBZ_CALL Notice(void*, const PbzNodeCall* call) {
    char text[64];
    snprintf(text, sizeof(text), "插件节点：数值 %d", call->value);
    api->notify(self, text);
}
void PBZ_CALL EndChain(void*, const PbzNodeCall*) { api->reset_chain(self, "end_chain"); }

PbzResult Decide(const char* id, const char* name, const char* description, const char* const* outputs, int32_t count, PbzDecideFn fn,
                 const char* label = nullptr, const char* unit = nullptr, int32_t lo = 0, int32_t hi = 0, int32_t step = 1, int32_t value = 0) {
    PbzActionDesc d{};
    d.size = sizeof(d); d.id = id; d.name = name; d.group = "条件判断"; d.description = description;
    d.kind = PBZ_ACTION_DECIDE; d.output_count = count; d.outputs = outputs; d.decide = fn;
    if (label) { d.has_value = 1; d.value_label = label; d.value_unit = unit; d.value_min = lo; d.value_max = hi; d.value_step = step; d.value_default = value; }
    return api->register_action(self, &d);
}
PbzResult Run(const char* id, const char* name, const char* description, PbzRunFn fn, bool value) {
    PbzActionDesc d{};
    d.size = sizeof(d); d.id = id; d.name = name; d.group = "动作"; d.description = description; d.kind = PBZ_ACTION_RUN; d.run = fn;
    if (value) { d.has_value = 1; d.value_label = "数值"; d.value_min = -1000; d.value_max = 1000; d.value_step = 1; d.value_default = 1; }
    return api->register_action(self, &d);
}
}  // namespace

PBZ_PLUGIN_EXPORT PbzResult PBZ_CALL PbzPluginMain(const PbzApi* a, PbzPlugin* s) {
    if (!a || a->version < 1) return PBZ_E_VERSION;
    api = a; self = s;
    PbzPluginInfo info{};
    info.size = sizeof(info); info.id = "pbz.conditions"; info.name = "条件判断（示例）"; info.version = "1.0.0"; info.author = "豪之刃";
    info.description = "按血量、距离、空中和概率分支的判断节点"; info.min_api = 1;
    if (PbzResult r = api->describe(self, &info); r != PBZ_OK) return r;

    static const char* const below[] = {"低于", "不低于"};
    static const char* const target_below[] = {"低于", "不低于", "没有目标"};
    static const char* const near_far[] = {"在距离内", "更远", "没有目标"};
    static const char* const air[] = {"空中", "地面"};
    static const char* const hit[] = {"命中", "未命中"};
    const PbzResult results[] = {
        Decide("hero_hp", "主角血量", "主角血量低于设定值时走第一个出口", below, 2, HeroHp, "主角血量低于", "%", 1, 100, 5, 30),
        Decide("target_hp", "目标血量", "锁定目标（或最近的敌人）血量低于设定值时走第一个出口", target_below, 3, TargetHp, "目标血量低于", "%", 1, 100, 5, 30),
        Decide("target_distance", "目标距离", "目标在设定距离内时走第一个出口", near_far, 3, TargetDistance, "距离", "厘米", 50, 5000, 50, 400),
        Decide("airborne", "空中", "主角在空中时走第一个出口", air, 2, Airborne),
        Decide("chance", "概率", "按设定概率走第一个出口", hit, 2, Chance, "命中概率", "%", 0, 100, 5, 50),
        Run("notice", "提示", "在画面右下角显示节点上的数值", Notice, true),
        Run("end_chain", "结束派生链", "走到这里时结束当前派生链", EndChain, false),
    };
    for (const PbzResult r : results) if (r != PBZ_OK) return r;
    api->log(self, "conditions ready: 5 decide actions, 2 run actions");
    return PBZ_OK;
}
