#pragma once

// Headless runs (run_audit.py --headless / --fast, 11-05): the game runs with -RenderOffScreen, so there is no
// window to click 继续游戏 on and the game counts as not focused. Command-file verbs, done on the game thread:
//   title continue      continues the save the start menu would (PBZSaveLoadSubsystem GetContinueSlotName +
//                       LoadGameOnStart, what WB_StartMenu's 继续游戏 runs); only on the start menu, once per 90 s
//   title quit          KismetSystemLibrary::QuitGame (a clean exit instead of a kill)
//   title keepalive on|off  while on, a pause nobody in this DLL set (the game pauses itself when it loses focus)
//                       is lifted again; never touches the hub's own pause
//   title probe         logs the game mode, the continue / load functions and their parameter names
namespace TitleControl {
using Logger = void (*)(const char*);
void Initialize(Logger logger);
bool HandleCommand(const char* verb, const char* argument);  // command-file thread; false: unknown argument
void OnGameThread();
}
