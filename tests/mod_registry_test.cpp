// 模块注册表 (ModRegistry) and the plugin UI-page model (PluginUi): descriptor tables, typed values, the thread rules, the
// guard around module functions, events, the command bus and the declared pages. No host, no window, no game.
#include "mod_registry.h"
#include "plugin_ui.h"
#include <cstdio>
#include <limits>
#include <string>
#include <thread>
#include <vector>

namespace {
int failures = 0;
void Check(bool ok, const char* what) {
    std::printf("%s %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) ++failures;
}
std::vector<std::string> Lines(const std::string& text) {
    std::vector<std::string> out; size_t at = 0;
    while (at <= text.size() && !text.empty()) {
        const size_t nl = text.find('\n', at);
        out.push_back(text.substr(at, nl == std::string::npos ? std::string::npos : nl - at));
        if (nl == std::string::npos) break;
        at = nl + 1;
    }
    return out;
}
std::vector<std::string> Cells(const std::string& line) {
    std::vector<std::string> out; size_t at = 0;
    while (true) {
        const size_t tab = line.find('\t', at);
        out.push_back(line.substr(at, tab == std::string::npos ? std::string::npos : tab - at));
        if (tab == std::string::npos) break;
        at = tab + 1;
    }
    return out;
}

// ---- a fake module ----
bool flag = false; int level = 3; float gain = 0.5f; int mode = 1; std::string title = "abc";
bool refuse = false; int calls = 0;
std::vector<std::string> logs;
void Sink(const char* text) { logs.push_back(text); }
using ModRegistry::Thread;
bool MakeFake(const char* name, Thread thread) {
    return ModRegistry::ModuleBuilder(name, "测试模块", "a\tfake\nmodule", thread)
        .Bool("flag", "开关", [] { return flag; }, [](bool v) { flag = v; }, "Fake.Flag")
        .Int("level", "等级", 1, 10, 3, [] { return level; }, [](int v) { level = v; ++calls; return !refuse; }, "Fake.Level", "1 到 10")
        .Float("gain", "增益", 0.0, 2.0, 0.5, [] { return gain; }, [](float v) { gain = v; })
        .Enum("mode", "模式", {"off", "low", "high"}, 1, [] { return mode; }, [](int v) { mode = v; })
        .Text("title", "标题", [] { return title; }, [](const std::string& v) { title = v; })
        .ReadOnlyInt("ticks", "计数", [] { return 42; })
        .Action("echo", "回声", "要回显的文字", false, [](const std::string& arg, std::string& out) { out = "echo:" + arg; return !arg.empty(); })
        .Event("changed", "变了", "属性名")
        .Commit();
}

// ---- the faulting module ----
int Crash() { volatile int* p = nullptr; return *p; }

// ---- a plugin page ----
PbzUiItem Item(int kind, const char* id, const char* label) { PbzUiItem i{}; i.size = sizeof(i); i.kind = kind; i.id = id; i.label = label; return i; }
}  // namespace

int main() {
    ModRegistry::SetLog(Sink);
    using ModRegistry::Type;

    // ---- registration and the tables ----
    Check(MakeFake("fake", Thread::Any), "a module registers");
    Check(!MakeFake("fake", Thread::Any), "the same name twice is refused");
    Check(!ModRegistry::ModuleBuilder("Bad Name", "", "", Thread::Game).Commit() && !ModRegistry::ModuleBuilder("", "", "", Thread::Game).Commit(), "bad module names are refused");
    Check(ModRegistry::ModuleBuilder("nogetter", "", "", Thread::Game).Commit(), "a module with no members is fine");
    {
        ModRegistry::Module m; m.name = "broken"; ModRegistry::Prop p; p.name = "x"; p.type = Type::Int; m.props.push_back(p);
        Check(!ModRegistry::Register(m), "a property without its accessors is refused");
        ModRegistry::Module e; e.name = "broken2"; ModRegistry::Prop q; q.name = "x"; q.type = Type::Enum; q.get_num = [](double& v) { v = 0; return true; }; q.read_only = true; e.props.push_back(q);
        Check(!ModRegistry::Register(e), "an enum without options is refused");
        ModRegistry::Module d; d.name = "dup"; ModRegistry::Prop a; a.name = "x"; a.type = Type::Bool; a.read_only = true; a.get_num = [](double& v) { v = 0; return true; }; d.props.push_back(a); d.props.push_back(a);
        Check(!ModRegistry::Register(d), "two properties of one name are refused");
        ModRegistry::Module act; act.name = "noact"; ModRegistry::Action x; x.name = "go"; act.actions.push_back(x);
        Check(!ModRegistry::Register(act), "an action without a function is refused");
    }
    ModRegistry::Unregister("nogetter");
    const std::vector<std::string> modules = Lines(ModRegistry::ModuleList());
    Check(modules.size() == 1 && Cells(modules[0]) == std::vector<std::string>{"fake", "测试模块", "any", "6", "1", "1", "a fake module"},
          "module_list: name, label, thread, counts, description (tabs and line breaks become spaces)");
    std::string text;
    Check(ModRegistry::PropList("fake", text) == PBZ_OK && Lines(text).size() == 6, "prop_list has a line per property");
    const std::vector<std::string> props = Lines(text);
    Check(Cells(props[0]) == std::vector<std::string>{"flag", "bool", "rw", "0", "1", "0", "Fake.Flag", "any", "开关", "", ""}, "a bool property's row");
    Check(Cells(props[1]) == std::vector<std::string>{"level", "int", "rw", "1", "10", "3", "Fake.Level", "any", "等级", "", "1 到 10"}, "an int property's row (range, default, where it is kept, note)");
    Check(Cells(props[2])[1] == "float" && Cells(props[2])[3] == "0" && Cells(props[2])[4] == "2" && Cells(props[2])[5] == "0.5", "a float property's row");
    Check(Cells(props[3])[1] == "enum" && Cells(props[3])[9] == "off,low,high" && Cells(props[3])[4] == "2", "an enum lists its names and ends at the last index");
    Check(Cells(props[4])[1] == "text" && Cells(props[5])[2] == "ro", "text and read-only rows");
    Check(ModRegistry::ActionList("fake", text) == PBZ_OK && Cells(Lines(text)[0]) == std::vector<std::string>{"echo", "any", "0", "要回显的文字", "回声", ""}, "action_list");
    Check(ModRegistry::EventList("fake", text) == PBZ_OK && Cells(Lines(text)[0]) == std::vector<std::string>{"changed", "变了", "属性名"}, "event_list");
    Check(ModRegistry::PropList("nope", text) == PBZ_E_NOT_FOUND && ModRegistry::Has("fake") && !ModRegistry::Has("nope") && ModRegistry::HasEvent("fake", "changed") && !ModRegistry::HasEvent("fake", "x"), "unknown modules are not found");

    // ---- values ----
    Check(ModRegistry::Get("fake", "flag", text, false) == PBZ_OK && text == "0" && ModRegistry::Get("fake", "level", text, false) == PBZ_OK && text == "3" &&
          ModRegistry::Get("fake", "gain", text, false) == PBZ_OK && text == "0.5" && ModRegistry::Get("fake", "mode", text, false) == PBZ_OK && text == "1" &&
          ModRegistry::Get("fake", "title", text, false) == PBZ_OK && text == "abc" && ModRegistry::Get("fake", "ticks", text, false) == PBZ_OK && text == "42", "get: the module's values as text");
    std::string canonical;
    Check(ModRegistry::Check("fake", "flag", "true", canonical) == PBZ_OK && canonical == "1" && ModRegistry::Check("fake", "flag", "off", canonical) == PBZ_OK && canonical == "0" &&
          ModRegistry::Check("fake", "flag", "2", canonical) == PBZ_E_ARG, "check: bool words");
    Check(ModRegistry::Check("fake", "level", "7", canonical) == PBZ_OK && canonical == "7" && ModRegistry::Check("fake", "level", "11", canonical) == PBZ_E_ARG &&
          ModRegistry::Check("fake", "level", "0", canonical) == PBZ_E_ARG && ModRegistry::Check("fake", "level", "3.5", canonical) == PBZ_E_ARG && ModRegistry::Check("fake", "level", "x", canonical) == PBZ_E_ARG,
          "check: an int must be a whole number inside its range (no silent clamp)");
    Check(ModRegistry::Check("fake", "gain", "1.25", canonical) == PBZ_OK && canonical == "1.25" && ModRegistry::Check("fake", "gain", "2.5", canonical) == PBZ_E_ARG &&
          ModRegistry::Check("fake", "gain", "nan", canonical) == PBZ_E_ARG && ModRegistry::Check("fake", "gain", "inf", canonical) == PBZ_E_ARG, "check: a float is finite and inside its range");
    Check(ModRegistry::Check("fake", "mode", "high", canonical) == PBZ_OK && canonical == "2" && ModRegistry::Check("fake", "mode", "HIGH", canonical) == PBZ_OK && canonical == "2" &&
          ModRegistry::Check("fake", "mode", "0", canonical) == PBZ_OK && canonical == "0" && ModRegistry::Check("fake", "mode", "3", canonical) == PBZ_E_ARG && ModRegistry::Check("fake", "mode", "mid", canonical) == PBZ_E_ARG,
          "check: an enum takes its index or its name");
    Check(ModRegistry::Check("fake", "title", "hello 世界", canonical) == PBZ_OK && canonical == "hello 世界" && ModRegistry::Check("fake", "title", std::string(600, 'x'), canonical) == PBZ_E_ARG &&
          ModRegistry::Check("fake", "title", "a\nb", canonical) == PBZ_E_ARG, "check: text has a length limit and no line breaks");
    Check(ModRegistry::Check("fake", "ticks", "1", canonical) == PBZ_E_ARG && ModRegistry::Check("fake", "none", "1", canonical) == PBZ_E_NOT_FOUND, "check: read-only is refused, unknown is not found");
    std::string why;
    Check(ModRegistry::Apply("fake", "level", "8", why) == PBZ_OK && level == 8 && ModRegistry::Apply("fake", "flag", "1", why) == PBZ_OK && flag &&
          ModRegistry::Apply("fake", "gain", "1.5", why) == PBZ_OK && gain == 1.5f && ModRegistry::Apply("fake", "mode", "2", why) == PBZ_OK && mode == 2 &&
          ModRegistry::Apply("fake", "title", "new", why) == PBZ_OK && title == "new", "apply calls the module's setters");
    refuse = true;
    Check(ModRegistry::Apply("fake", "level", "5", why) == PBZ_E_STATE && why.find("could not") != std::string::npos && ModRegistry::Apply("fake", "level", "50", why) == PBZ_E_ARG, "a setter that says no is PBZ_E_STATE; a bad value never reaches it");
    refuse = false;
    Check(ModRegistry::Invoke("fake", "echo", "hi", text) == PBZ_OK && text == "echo:hi" && ModRegistry::Invoke("fake", "echo", "", text) == PBZ_E_STATE && text == "echo:" &&
          ModRegistry::Invoke("fake", "none", "", text) == PBZ_E_NOT_FOUND, "actions: a text result; a failing one still returns its text");

    // ---- threads ----
    Check(MakeFake("gamey", Thread::Game), "a Game module");
    Check(ModRegistry::Get("gamey", "level", text, true) == PBZ_OK && ModRegistry::Get("gamey", "level", text, false) == PBZ_E_THREAD && ModRegistry::Get("fake", "level", text, false) == PBZ_OK,
          "a Game module is only read on the game thread; an Any module anywhere");
    Check(MakeFake("worky", Thread::Worker) && ModRegistry::Get("worky", "level", text, false) == PBZ_OK, "a Worker module's reads are free");

    // ---- a module that faults ----
    {
        ModRegistry::ModuleBuilder b("crashy", "会崩", "", Thread::Any);
        b.Int("v", "v", 0, 1, 0, [] { return Crash(); }, [](int) {}).Int("w", "w", 0, 1, 0, [] { return 0; }, [](int) {})
         .Action("boom", "", "", false, [](const std::string&, std::string&) -> bool { Crash(); return true; });
        Check(b.Commit(), "the faulting module registers");
    }
    Check(ModRegistry::Get("crashy", "v", text, true) == PBZ_E_STATE && ModRegistry::Get("crashy", "w", text, true) == PBZ_E_STATE && ModRegistry::Apply("crashy", "w", "1", why) == PBZ_E_STATE &&
          ModRegistry::Invoke("crashy", "boom", "", text) == PBZ_E_STATE && ModRegistry::Get("fake", "level", text, true) == PBZ_OK,
          "a fault in a module ends the call and switches that module off; the others carry on");
    bool logged = false; for (const std::string& l : logs) logged = logged || l.find("MODULE [crashy] fault") != std::string::npos;
    Check(logged, "the fault is logged");

    // ---- events ----
    ModRegistry::Emit("fake", "changed", "level");
    Check(ModRegistry::Drain().empty(), "events are dropped while nobody listens");
    ModRegistry::SetListening(true);
    std::thread([] { ModRegistry::Emit("fake", "changed", "from another thread"); }).join();
    ModRegistry::Emit("fake", "changed", std::string(500, 'z'));
    const std::vector<ModRegistry::Emitted> raised = ModRegistry::Drain();
    Check(raised.size() == 2 && raised[0].text == "from another thread" && raised[1].text.size() == 200 && raised[0].module == "fake" && raised[0].event == "changed" && ModRegistry::Drain().empty(),
          "events from any thread are queued, clipped, and drained once");

    // ---- pollers: a module with no event hook raises its events from its snapshot ----
    {
        static int polls = 0, boom = 0;
        Check(ModRegistry::AddPoller("fake", [] { ++polls; ModRegistry::Emit("fake", "changed", "poll"); }) && !ModRegistry::AddPoller("nomod", [] {}) && !ModRegistry::AddPoller("fake", nullptr),
              "a poller needs a registered module and a function");
        ModRegistry::SetListening(false); ModRegistry::RunPollers();
        Check(polls == 0, "pollers do not run while nobody listens");
        ModRegistry::SetListening(true); ModRegistry::RunPollers();
        const std::vector<ModRegistry::Emitted> polled = ModRegistry::Drain();
        Check(polls == 1 && polled.size() == 1 && polled[0].text == "poll", "...and run once a frame while someone does, their events queued like any other");
        ModRegistry::AddPoller("fake", [] { ++boom; Crash(); });
        ModRegistry::RunPollers(); ModRegistry::RunPollers(); ModRegistry::Drain();
        Check(boom == 1 && polls == 3, "a poller that faults is dropped; the others carry on");
    }

    // ---- commands ----
    ModRegistry::Command diag; diag.name = "fake.status"; diag.args = "[verbose]"; diag.note = "prints"; diag.kind = ModRegistry::Kind::Diagnostic; diag.thread = Thread::Game;
    diag.run = [](const std::string& args, std::string& out) { out = "status " + args; return true; };
    ModRegistry::Command bad = diag; bad.name = "nodot";
    ModRegistry::Command nofn = diag; nofn.name = "fake.nofn"; nofn.run = nullptr;
    Check(ModRegistry::RegisterCommand(diag) && !ModRegistry::RegisterCommand(diag) && !ModRegistry::RegisterCommand(bad) && !ModRegistry::RegisterCommand(nofn), "commands: a name like module.verb, a function, no duplicates");
    ModRegistry::Command save = diag; save.name = "fake.save"; save.kind = ModRegistry::Kind::Persist; save.thread = Thread::Worker; save.run = [](const std::string&, std::string& out) { out = "saved"; return true; };
    ModRegistry::Command fail = diag; fail.name = "fake.fail"; fail.kind = ModRegistry::Kind::State; fail.run = [](const std::string&, std::string& out) { out = "not now"; return false; };
    ModRegistry::Command boom = diag; boom.name = "fake.boom"; boom.run = [](const std::string&, std::string&) -> bool { Crash(); return true; };
    ModRegistry::RegisterCommand(save); ModRegistry::RegisterCommand(fail); ModRegistry::RegisterCommand(boom);
    const std::vector<std::string> commands = Lines(ModRegistry::CommandList());
    Check(commands.size() == 4 && Cells(commands[0]) == std::vector<std::string>{"fake.boom", "game", "diagnostic", "[verbose]", "prints"} && Cells(commands[3])[0] == "fake.status" &&
          Cells(commands[2])[1] == "worker" && Cells(commands[2])[2] == "persist", "command_list: name, thread, kind, args, note (sorted)");
    Check(ModRegistry::RunCommand("fake.status", "x y", text) == PBZ_OK && text == "status x y" && ModRegistry::RunCommand("fake.fail", "", text) == PBZ_E_STATE && text == "not now" &&
          ModRegistry::RunCommand("fake.none", "", text) == PBZ_E_NOT_FOUND && ModRegistry::RunCommand("fake.boom", "", text) == PBZ_E_STATE && ModRegistry::RunCommand("fake.status", "", text) == PBZ_OK,
          "commands run behind the guard: failures carry their text, a crash does not escape");
    {
        static std::vector<std::string> done; static int allowed = 1; done.clear();
        Check(ModRegistry::PostWorker({"owner1", "fake.save", "", "fake.save"}) && ModRegistry::PostWorker({"owner2", "fake.save", "", "fake.save now"}) && ModRegistry::PostWorker({"owner1", "fake.status", "a", "fake.status a"}), "worker jobs are queued");
        const size_t ran = ModRegistry::PumpWorker([](const ModRegistry::WorkerJob& job) { return job.owner == "owner1" || allowed == 0; },
            [](const std::string& owner, bool ok, const std::string& line, const std::string& out) { done.push_back(owner + "|" + (ok ? "ok" : "fail") + "|" + line + "|" + out); });
        Check(ran == 3 && done.size() == 3 && done[0] == "owner1|ok|fake.save|saved" && done[1] == "owner2|fail|fake.save now|denied" && done[2] == "owner1|ok|fake.status a|status a" && ModRegistry::PumpWorker(nullptr, nullptr) == 0,
              "the panel thread pumps them: a job whose owner lost the permission is answered \"denied\" and not run");
        bool full = false; for (int k = 0; k < 70; ++k) if (!ModRegistry::PostWorker({"o", "fake.save", "", "x"})) full = true;
        Check(full, "the worker queue is bounded");
        ModRegistry::PumpWorker(nullptr, nullptr);
    }

    // ---- UI pages ----
    {
        PbzUiItem label = Item(PBZ_UI_LABEL, "info", "状态"); label.text = "ready";
        PbzUiItem toggle = Item(PBZ_UI_TOGGLE, "on", "启用"); toggle.value = 5;
        PbzUiItem slider = Item(PBZ_UI_SLIDER, "amt", "数量"); slider.min = 0; slider.max = 10; slider.step = 0; slider.value = 99; slider.text = "个";
        PbzUiItem choice = Item(PBZ_UI_CHOICE, "pick", "选择"); choice.text = "甲, 乙,丙"; choice.value = 1;
        PbzUiItem button = Item(PBZ_UI_BUTTON, "go", "执行");
        PbzUiItem multi = Item(PBZ_UI_TEXT, "log", ""); multi.text = "line1\nline2";
        const PbzUiItem* items[] = {&label, &toggle, &slider, &choice, &button, &multi};
        PbzUiPage page{}; page.size = sizeof(page); page.id = "main"; page.title = "我的页"; page.items = items; page.item_count = 6;
        Check(PluginUi::Declare("plug", L"插件", page) == PBZ_OK, "a page of every kind is declared");
        const std::vector<PluginUi::Page> pages = PluginUi::Snapshot();
        Check(pages.size() == 1 && pages[0].owner == "plug" && pages[0].title == L"我的页" && pages[0].items.size() == 6 && pages[0].items[1].value == 1 && pages[0].items[2].value == 10 && pages[0].items[2].step == 0.1 &&
              pages[0].items[3].choices.size() == 3 && pages[0].items[3].choices[0] == L"甲" && pages[0].items[3].choices[1] == L"乙" && pages[0].items[3].max == 2 && pages[0].items[5].text == L"line1\nline2",
              "values are clamped, a missing step is a hundredth of the range, choices split on commas");
        const uint64_t serial = PluginUi::Serial();
        Check(PluginUi::SetValue("plug", "main", "amt", 4.5) == PBZ_OK && PluginUi::SetValue("plug", "main", "amt", -3) == PBZ_OK && PluginUi::SetValue("plug", "main", "pick", 7) == PBZ_OK && PluginUi::SetValue("plug", "main", "on", 2) == PBZ_OK,
              "ui_set_value");
        PluginUi::Item got;
        Check(PluginUi::FindItem("plug", "main", "amt", got) && got.value == 0 && PluginUi::FindItem("plug", "main", "pick", got) && got.value == 2 && PluginUi::FindItem("plug", "main", "on", got) && got.value == 1 && PluginUi::Serial() != serial,
              "the shown values follow (a choice stays an index, a toggle 0 / 1)");
        Check(PluginUi::SetValue("plug", "main", "info", 1) == PBZ_E_ARG && PluginUi::SetValue("plug", "main", "nope", 1) == PBZ_E_NOT_FOUND && PluginUi::SetValue("plug", "other", "on", 1) == PBZ_E_NOT_FOUND && PluginUi::SetValue("plug", "main", "on", std::numeric_limits<double>::infinity()) == PBZ_E_ARG,
              "a label has no value; unknown items and pages; a non-finite value");
        Check(PluginUi::SetText("plug", "main", "info", L"busy") == PBZ_OK && PluginUi::SetText("plug", "main", "go", L"再来一次") == PBZ_OK && PluginUi::SetText("plug", "main", "on", L"x") == PBZ_E_ARG &&
              PluginUi::FindItem("plug", "main", "info", got) && got.text == L"busy" && PluginUi::FindItem("plug", "main", "go", got) && got.label == L"再来一次", "ui_set_text: a label's text, a button's caption");

        PbzUiPage same = page; same.item_count = 1;
        Check(PluginUi::Declare("plug", L"插件", same) == PBZ_OK && PluginUi::Snapshot().size() == 1 && PluginUi::Snapshot()[0].items.size() == 1, "the same id again replaces the page");
        PbzUiItem twin = Item(PBZ_UI_LABEL, "info", "x"); const PbzUiItem* twins[] = {&label, &twin};
        PbzUiPage dup = page; dup.items = twins; dup.item_count = 2;
        PbzUiItem badid = Item(PBZ_UI_LABEL, "a b", "x"); const PbzUiItem* badids[] = {&badid}; PbzUiPage bp = page; bp.items = badids; bp.item_count = 1;
        PbzUiItem badkind = Item(77, "k", "x"); const PbzUiItem* badkinds[] = {&badkind}; PbzUiPage bk = page; bk.items = badkinds; bk.item_count = 1;
        PbzUiItem flat = Item(PBZ_UI_SLIDER, "s", "x"); flat.min = 5; flat.max = 5; const PbzUiItem* flats[] = {&flat}; PbzUiPage fp = page; fp.items = flats; fp.item_count = 1;
        PbzUiItem nocap = Item(PBZ_UI_BUTTON, "b", ""); const PbzUiItem* nocaps[] = {&nocap}; PbzUiPage nc = page; nc.items = nocaps; nc.item_count = 1;
        PbzUiPage zero = page; zero.item_count = 0; PbzUiPage nid = page; nid.id = "bad id";
        Check(PluginUi::Declare("plug", L"", dup) == PBZ_E_ARG && PluginUi::Declare("plug", L"", bp) == PBZ_E_ARG && PluginUi::Declare("plug", L"", bk) == PBZ_E_ARG && PluginUi::Declare("plug", L"", fp) == PBZ_E_ARG &&
              PluginUi::Declare("plug", L"", nc) == PBZ_E_ARG && PluginUi::Declare("plug", L"", zero) == PBZ_E_ARG && PluginUi::Declare("plug", L"", nid) == PBZ_E_ARG, "duplicate item ids, bad ids, kinds, a flat slider, a captionless button, no items, a bad page id are refused");
        for (int k = 1; k <= 7; ++k) { std::string id = "p" + std::to_string(k); PbzUiPage p = page; p.id = id.c_str(); PluginUi::Declare("plug", L"", p); }
        PbzUiPage ninth = page; ninth.id = "p9";
        Check(PluginUi::Declare("plug", L"", ninth) == PBZ_E_FULL && PluginUi::Declare("other", L"", ninth) == PBZ_OK, "at most 8 pages per plugin");
        PluginUi::RemoveOwner("plug");
        Check(PluginUi::Snapshot().size() == 1 && PluginUi::Snapshot()[0].owner == "other" && PluginUi::Remove("other", "p9") == PBZ_OK && PluginUi::Remove("other", "p9") == PBZ_E_NOT_FOUND && PluginUi::Snapshot().empty(), "remove a page; remove an owner's pages");

        // binds
        PbzUiItem btog = Item(PBZ_UI_TOGGLE, "t", "开关"); btog.bind = "fake.flag";
        PbzUiItem bsl = Item(PBZ_UI_SLIDER, "s", "等级"); bsl.bind = "fake.level";
        PbzUiItem bsf = Item(PBZ_UI_SLIDER, "f", "增益"); bsf.bind = "fake.gain";
        PbzUiItem bch = Item(PBZ_UI_CHOICE, "c", "模式"); bch.bind = "fake.mode";
        PbzUiItem blb = Item(PBZ_UI_LABEL, "l", "标题"); blb.bind = "fake.title";
        PbzUiItem bro = Item(PBZ_UI_LABEL, "r", "计数"); bro.bind = "fake.ticks";
        PbzUiItem bbt = Item(PBZ_UI_BUTTON, "b", "回声"); bbt.bind = "fake:echo"; bbt.text = "yo";
        const PbzUiItem* bound[] = {&btog, &bsl, &bsf, &bch, &blb, &bro, &bbt};
        PbzUiPage bpage = page; bpage.id = "bound"; bpage.items = bound; bpage.item_count = 7;
        Check(PluginUi::Declare("plug", L"", bpage) == PBZ_OK, "controls bound to module properties and an action are declared");
        const PluginUi::Page b = PluginUi::Snapshot()[0];
        Check(b.items[1].min == 1 && b.items[1].max == 10 && b.items[1].step == 1 && b.items[2].min == 0 && b.items[2].max == 2 && b.items[2].step == 0.02 && b.items[3].choices.size() == 3 && b.items[3].choices[2] == L"high",
              "a bound control takes its range and choices from the property");
        Check(PluginUi::Bindings().size() == 6, "every bound item but the button is refreshed by the host");
        PbzUiItem wrong1 = Item(PBZ_UI_TOGGLE, "x", "x"); wrong1.bind = "fake.level";   // a toggle on an int
        PbzUiItem wrong2 = Item(PBZ_UI_SLIDER, "x", "x"); wrong2.bind = "fake.title";   // a slider on text
        PbzUiItem wrong3 = Item(PBZ_UI_BUTTON, "x", "x"); wrong3.bind = "fake.level";   // a button on a property
        PbzUiItem wrong4 = Item(PBZ_UI_TOGGLE, "x", "x"); wrong4.bind = "fake:echo";    // a toggle on an action
        PbzUiItem wrong5 = Item(PBZ_UI_SLIDER, "x", "x"); wrong5.bind = "fake.ticks";   // a control on a read-only property
        PbzUiItem wrong6 = Item(PBZ_UI_TOGGLE, "x", "x"); wrong6.bind = "nomodule.flag";
        PbzUiItem wrong7 = Item(PBZ_UI_TOGGLE, "x", "x"); wrong7.bind = "fake.nothing";
        PbzUiItem wrong8 = Item(PBZ_UI_TOGGLE, "x", "x"); wrong8.bind = "fake";
        const PbzUiItem* wrongs[] = {&wrong1, &wrong2, &wrong3, &wrong4, &wrong5, &wrong6, &wrong7, &wrong8};
        int refused = 0, notfound = 0;
        for (const PbzUiItem* w : wrongs) { const PbzUiItem* one[] = {w}; PbzUiPage p = page; p.id = "w"; p.items = one; p.item_count = 1; const PbzResult r = PluginUi::Declare("plug", L"", p); refused += r == PBZ_E_ARG; notfound += r == PBZ_E_NOT_FOUND; }
        Check(refused == 6 && notfound == 2, "a bind must name a registered property (or action for a button) of a matching type; read-only ones only show");
        PluginUi::SetShown("plug", "bound", "t", 1, L"", false); PluginUi::SetShown("plug", "bound", "l", 0, L"hello", true);
        PluginUi::Item t, l;
        Check(PluginUi::FindItem("plug", "bound", "t", t) && t.value == 1 && PluginUi::FindItem("plug", "bound", "l", l) && l.text == L"hello", "the host shows a bound item's live value");
        PluginUi::ResetForTest();
        Check(PluginUi::Snapshot().empty(), "reset");
    }

    ModRegistry::ResetForTest();
    Check(ModRegistry::ModuleList().empty() && ModRegistry::CommandList().empty(), "reset empties the tables");
    std::printf("%s (%d failure(s))\n", failures ? "FAILED" : "ALL PASSED", failures);
    return failures ? 1 : 0;
}
