#pragma once
#include "air_skills.h"
#include <windows.h>

// Native Win32 UI on the existing worker. UObject work is submitted to AirSkills.
namespace AirSkillsMenu {
void Initialize(AirSkills::Logger logger);
HWND CreatePage(HWND parent);
void ToggleWindow();
void Tick();
}
