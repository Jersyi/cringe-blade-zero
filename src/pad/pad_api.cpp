// 手柄 in the module registry (module "pad"): the adaptive-trigger switch. PadTriggers keeps it in the ini itself.
#include "mod_registry.h"
#include "module_apis.h"
#include "pad_triggers.h"
#include "plugin_text.h"
#include <string>

namespace ModuleApis {
void RegisterPad() {
    using ModRegistry::ModuleBuilder;
    using ModRegistry::Thread;
    ModuleBuilder("pad", "手柄", "DualSense 自适应扳机：强制关闭（改写发给手柄的报告和 Steam Input 的扳机效果）", Thread::Any)
        .Bool("triggers_forced_off", "强制关闭自适应扳机", [] { return PadTriggers::ForcedOff(); }, [](bool v) { PadTriggers::SetForcedOff(v); },
              "Controller.ForceAdaptiveTriggersOff", "写 ini")
        .ReadOnlyText("triggers_status", "扳机状态", [] { wchar_t text[256]{}; PadTriggers::Status(text, 256); return PluginText::Utf8(text); })
        .Commit();
}
}  // namespace ModuleApis
