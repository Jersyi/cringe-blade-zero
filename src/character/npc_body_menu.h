#pragma once
#include <windows.h>

// 身材 / 骨骼 页（代替「骨骼变换」页）：总开关、打开 UUEPBS 编辑器的按钮和状态。编辑本身在 NpcBridge 的窗口里。
// 面板线程（DebugHub worker）调用。
namespace NpcBodyMenu {
using Logger = void (*)(const char*);
void Initialize(Logger logger = nullptr);
HWND CreateLauncherPage(HWND parent);
void Tick();
void Show();      // the page, or (no panel) the editor itself
void Shutdown();  // also closes the editor window
}
