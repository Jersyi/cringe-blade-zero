#pragma once
#include <windows.h>
#include <functional>
#include <string>
#include <vector>
#include "npc_combo_model.h"

// 加速 曲线调节's own window (招式派生 page): one 加速 node's curve over its moves, edited with the mouse. X: how far the
// move is (percent of its montage), Y: its speed (×0.5 to ×10, log scale). Drag a point (the two ends move up and down
// only; Shift: 1 % steps, else 5 %), double-click to add one, right-click to remove one, the wheel over a point nudges
// it; presets below. It floats over the page as a child of the hub (so it fades and moves with it, and the game never
// shows through it) and is dragged by its title; it closes
// with its × button, when the page hides, or when its node is gone.
namespace SpeedCurveWindow {
struct Host {
    // The node's curve now, whether 曲线调节 is on, its one speed (percent) and a title; false: the node is gone.
    std::function<bool(std::vector<NpcCombo::CurvePoint>& points, bool& on, int& speed, std::wstring& title)> read;
    // A changed curve (and 曲线调节 on / off); commit: the drag or click is over (saved then).
    std::function<void(const std::vector<NpcCombo::CurvePoint>& points, bool on, bool commit)> write;
};
// UI worker only.
void Open(HWND page, int node, Host host);
int EditedNode();   // the node it edits (-1: closed)
void Close(const char* why = "closed");
const char* LastClose();  // why it last closed (COMBO_CURVE log lines)
void Tick(HWND page);  // every page tick: re-reads the node, closes with the page, stays above the hub
// Tests (combo_menu_preview): the window, a point's middle in its client, the graph's rect, and a client point's curve value.
HWND Window();
POINT PointAt(size_t index);
RECT Graph();
}
