#pragma once

// UI worker only. A gear dial (0-10) in the bottom-right of the game client,
// styled after the game's menus. Higher gears turn on more assists: JC and an
// easier JC trigger range, grabs, JC rewards, grab damage and finally ignoring
// Boss invulnerability. Shown while the hub is open, and briefly after a
// gear hotkey. While adjusting, a bubble shows the cold-to-grin face.
namespace GearWidget {
using Logger = void (*)(const char*);
void Initialize(Logger logger);
void Tick();
// Hotkeys: step the gear and flash the dial.
void Nudge(int delta);
int Level();
bool Active();
void Shutdown();
}
