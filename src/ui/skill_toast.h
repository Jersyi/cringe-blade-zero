#pragma once
#include <string>

// The boss skill an NPC skin just cast, as a short notice in the bottom-right of the game client
// (above the gear pill), styled like the hub's own notices. Show: any thread, copies the text and
// makes no window call; the UI worker draws it on its next Tick. Tick / Active / Shutdown: UI worker.
namespace SkillToast {
void Show(const std::wstring& title, const std::wstring& detail);
void Tick();
bool Active();
void Shutdown();
}
