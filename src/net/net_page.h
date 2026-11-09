#pragma once
#include <windows.h>

// UI worker only. Hub page 联机测试 (online probe, phase 0): Steam room, invites, room number, the link's state, and
// the puppet. Requests go to NetSteam / NetPuppet (done on the game thread); their snapshots are shown here.
namespace NetPage {
HWND CreatePage(HWND parent);
void Tick();
void Shutdown();
// Worker thread, the command file (`net host|overlay|leave|ping|friends|spawn|remove|mirror 0/1|join <room>|invite <steam id>`).
bool HandleCommand(const char* argument);
}
