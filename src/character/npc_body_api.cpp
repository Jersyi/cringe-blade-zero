// 身材 / 骨骼 in the module registry: module "npc_body" (the UUEPBS editor's live data: bone edits by name, morph weights, presets, the
// switch and the editor window) and module "bones" (the 骨骼变换 runtime that hooks the pose: status, supplement, debug, and the
// pose_finalized event). Uses uuepbs::Registry (thread safe: the editor window, the pose hook and the game thread share it) and
// NpcBridge / BoneMotionRuntime's public headers; their code does not change for this.
#include "bone_motion.h"
#include "mod_registry.h"
#include "module_apis.h"
#include "npc_bridge.h"
#include "npc_retarget.h"
#include "npc_body.h"
#include "plugin_text.h"
#include "registry.hpp"
#include <atomic>
#include <cmath>
#include <cstdio>
#include <string>

namespace ModuleApis {
namespace {
using PluginText::ToDouble;
using PluginText::Words;

std::string Num(double v) { char t[40]{}; _snprintf_s(t, sizeof(t), _TRUNCATE, "%.4g", v); return t; }
uuepbs::Registry& Reg() { return uuepbs::Registry::instance(); }

// "bone sx sy sz [rx ry rz [tx ty tz]]": scale along the bone's own X (length), Y (width), Z (depth), then optional turn (degrees)
// and shift (cm); the edit is clamped to the editor's own limits.
bool SetBone(const std::string& arg, std::string& out) {
    const std::vector<std::string> w = Words(arg);
    if (w.size() != 4 && w.size() != 7 && w.size() != 10) { out = "格式：骨骼名 长 宽 深 [转X 转Y 转Z [移X 移Y 移Z]]（缩放倍数；转角为度，位移为厘米）"; return false; }
    uuepbs::BoneEdit edit;
    for (size_t i = 1; i < w.size(); ++i) {
        double v = 0;
        if (!ToDouble(w[i], v)) { out = "不是数字：" + w[i]; return false; }
        if (i <= 3) edit.axis[i - 1] = v; else if (i <= 6) edit.turn[i - 4] = v; else edit.shift[i - 7] = v;
    }
    edit.clamp();
    Reg().set_bone(w[0], edit);
    out = w[0] + " 长" + Num(edit.axis[0]) + " 宽" + Num(edit.axis[1]) + " 深" + Num(edit.axis[2]);
    return true;
}
bool ListBones(std::string& out) {
    for (const auto& [key, entry] : Reg().edits()) {
        (void)key;
        out += entry.bone + "\t" + Num(entry.edit.axis[0]) + "\t" + Num(entry.edit.axis[1]) + "\t" + Num(entry.edit.axis[2]) + "\t" + Num(entry.edit.turn[0]) + "\t" + Num(entry.edit.turn[1]) + "\t" +
               Num(entry.edit.turn[2]) + "\t" + Num(entry.edit.shift[0]) + "\t" + Num(entry.edit.shift[1]) + "\t" + Num(entry.edit.shift[2]) + "\n";
    }
    return true;
}
bool SetMorph(const std::string& arg, std::string& out) {
    const std::vector<std::string> w = Words(arg);
    double weight = 0;
    if (w.size() != 2 || !ToDouble(w[1], weight)) { out = "格式：形变名 权重（-2 到 2）"; return false; }
    Reg().set_morph(w[0], uuepbs::clamp_morph(weight));
    out = w[0] + " = " + Num(uuepbs::clamp_morph(weight));
    return true;
}
bool Preset(const std::string& arg, std::string& out) {
    const std::vector<std::string> w = Words(arg);
    if (w.size() < 2) { out = "格式：save|load|delete 预设名"; return false; }
    std::string name = w[1]; for (size_t i = 2; i < w.size(); ++i) name += " " + w[i];
    std::string message; bool ok = false;
    if (w[0] == "save") ok = NpcBridge::PresetSave(name, message);
    else if (w[0] == "load") ok = NpcBridge::PresetLoad(name, message);
    else if (w[0] == "delete") ok = NpcBridge::PresetRemove(name, message);
    else { out = "格式：save|load|delete 预设名"; return false; }
    out = message;
    return ok;
}

// ---- the pose event: a counter bumped on the finalizing thread (nothing else is allowed there), read by a poller on the game thread ----
std::atomic<long> g_poses{0};
void OnPose(ue::Object*) { g_poses.fetch_add(1, std::memory_order_relaxed); }
void PollPose() {
    static ue::Object* target = nullptr; static int id = 0; static long seen = 0;
    ue::Object* want = static_cast<ue::Object*>(NpcBody::HookedComponent());
    if (!want) want = NpcRetarget::BoneBody();  // the body that is shown (the hero's, or the NPC look's display)
    if (want != target) {
        if (id) BoneMotionRuntime::RemoveListener(id);
        id = 0; target = want;
        if (want) id = BoneMotionRuntime::AddListener(want, &OnPose);
    }
    const long now = g_poses.load(std::memory_order_relaxed);
    if (now != seen) { seen = now; ModRegistry::Emit("bones", "pose_finalized", std::to_string(now)); }
}
}  // namespace

void RegisterBody() {
    using ModRegistry::ModuleBuilder;
    using ModRegistry::Thread;
    ModuleBuilder("npc_body", "身材 / 骨骼", "UUEPBS 编辑器的数据：按骨骼名的缩放 / 旋转 / 位移、形变权重、预设。编辑器自己的窗口和这里改的是同一份数据；改动在下一次姿势计算时生效。"
                  "主角变身成 BOSS 期间编辑暂停", Thread::Any)
        .Bool("enabled", "启用身材 / 骨骼", [] { return NpcBridge::Enabled(); }, [](bool v) { NpcBridge::SetEnabled(v); }, "NpcBody.Enabled", "关闭时不碰主角的身体。打开编辑器会自动打开")
        .Bool("edits_on", "编辑生效", [] { return Reg().enabled(); }, [](bool v) { Reg().set_enabled(v); }, "", "只是这次游戏的开关，不保存")
        .ReadOnlyBool("editor_started", "编辑器窗口已创建", [] { return NpcBridge::Started(); })
        .ReadOnlyBool("editor_visible", "编辑器窗口显示中", [] { return NpcBridge::Visible(); })
        .ReadOnlyInt("bones_edited", "改过的骨骼数", [] { return static_cast<int>(Reg().edits().size()); })
        .ReadOnlyInt("morphs_set", "设了权重的形变数", [] { return static_cast<int>(Reg().morphs().size()); })
        .ReadOnlyInt("rigs", "跟踪的骨架数", [] { return static_cast<int>(Reg().rigs().size()); })
        .ReadOnlyText("hook", "姿势钩子", [] { return std::string(NpcBody::HookReport()); })
        .Action("bone", "改一根骨骼", "骨骼名 长 宽 深 [转X 转Y 转Z [移X 移Y 移Z]]", false, [](const std::string& arg, std::string& out) { return SetBone(arg, out); })
        .Action("clear_bone", "清掉一根骨骼的改动", "骨骼名", false, [](const std::string& arg, std::string& out) {
            const std::vector<std::string> w = Words(arg); if (w.size() != 1) { out = "格式：骨骼名"; return false; }
            Reg().clear_bone(w[0]); return true; })
        .Action("clear_bones", "清掉全部骨骼改动", "", false, [](const std::string&, std::string&) { Reg().clear_edits(); return true; })
        .Action("bones", "骨骼改动列表", "", false, [](const std::string&, std::string& out) { return ListBones(out); })
        .Action("morph", "设形变权重", "形变名 权重", false, [](const std::string& arg, std::string& out) { return SetMorph(arg, out); })
        .Action("clear_morph", "清掉一个形变", "形变名", false, [](const std::string& arg, std::string& out) {
            const std::vector<std::string> w = Words(arg); if (w.size() != 1) { out = "格式：形变名"; return false; }
            Reg().clear_morph(w[0]); return true; })
        .Action("clear_morphs", "清掉全部形变", "", false, [](const std::string&, std::string&) { Reg().clear_morphs(); return true; })
        .Action("morphs", "形变列表", "", false, [](const std::string&, std::string& out) {
            for (const auto& [key, entry] : Reg().morphs()) { (void)key; out += entry.name + "\t" + Num(entry.weight) + "\n"; }
            return true; })
        .Action("preset", "存 / 载入 / 删除预设", "save|load|delete 预设名", true, [](const std::string& arg, std::string& out) { return Preset(arg, out); })
        .Action("presets", "预设列表", "", false, [](const std::string&, std::string& out) {
            for (const std::string& name : NpcBridge::PresetNames()) out += name + "\n";
            return true; })
        .Action("editor", "编辑器窗口", "show | hide | toggle", false, [](const std::string& arg, std::string& out) {
            if (arg == "show") NpcBridge::Start(true);
            else if (arg == "hide") NpcBridge::Stop();
            else if (arg == "toggle") NpcBridge::Toggle();
            else { out = "格式：show | hide | toggle"; return false; }
            return true; })
        .Commit();

    const bool bones = ModuleBuilder("bones", "骨骼变换运行时", "把姿势钩子装在主角（或外观）的身体上：骨骼变换节点、补充骨骼、碰撞。pose_finalized 事件在引擎每算完一次姿势后（合并成每帧一次）发出",
                                     Thread::Worker)
        .Bool("supplement", "补充骨骼", [] { return BoneMotionRuntime::SupplementEnabled(); }, [](bool v) { BoneMotionRuntime::Supplement(v, true); }, "BoneMotion.Supplement")
        .Bool("debug", "骨骼调试显示", [] { return BoneMotionRuntime::Get().debug; }, [](bool v) { BoneMotionRuntime::Debug(v); }, "", "不保存")
        .ReadOnlyBool("listening", "姿势钩子已装上", [] { return BoneMotionRuntime::Listening(); })
        .ReadOnlyText("listen_note", "钩子说明", [] { return std::string(BoneMotionRuntime::ListenNote()); })
        .ReadOnlyInt("hooks", "钩子数", [] { return BoneMotionRuntime::Get().hooks; })
        .ReadOnlyInt("matched", "匹配的骨骼数", [] { return BoneMotionRuntime::Get().matched; })
        .ReadOnlyText("mesh", "网格", [] { return BoneMotionRuntime::Get().mesh; })
        .Event("pose_finalized", "姿势算完", "累计次数（每帧最多一个事件）")
        .Commit();
    if (bones) ModRegistry::AddPoller("bones", &PollPose);

    ModRegistry::Command status; status.name = "bones.status"; status.kind = ModRegistry::Kind::Diagnostic; status.thread = Thread::Worker;
    status.note = "钩子、匹配、缺失的骨骼和帧数（同时写日志）";
    status.run = [](const std::string&, std::string& out) {
        const BoneMotionRuntime::Snapshot s = BoneMotionRuntime::Get();
        out = "mesh=" + s.mesh + " hooks=" + std::to_string(s.hooks) + " matched=" + std::to_string(s.matched) + " missing=" + std::to_string(s.missing.size()) +
              " frames=" + std::to_string(s.frames) + " note=" + s.note;
        return BoneMotionRuntime::Command("status");
    };
    ModRegistry::RegisterCommand(status);
    ModRegistry::Command probe; probe.name = "bones.collisionprobe"; probe.kind = ModRegistry::Kind::Diagnostic; probe.thread = Thread::Worker;
    probe.note = "下一帧把骨骼碰撞的探测结果写进日志";
    probe.run = [](const std::string&, std::string& out) { out = "写进日志"; return BoneMotionRuntime::Command("collisionprobe"); };
    ModRegistry::RegisterCommand(probe);
}
}  // namespace ModuleApis
