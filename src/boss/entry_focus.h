#pragma once

// Window-only focus sampling. Initialize/Poll run on the existing worker;
// the game thread uses the getters, which perform only atomic loads.
// Unknown, external-process, modal-game and absent foreground windows do not
// count as gameplay focus. This module never changes focus or pauses the game.
namespace EntryFocus {
void Initialize();
void Poll();
bool MenuFocused() noexcept;
bool GameplayFocused() noexcept;
}
