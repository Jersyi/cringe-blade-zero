#pragma once
#include <windows.h>
#include <string>
#include <vector>
#include <cstdint>

// UI worker only. Hub page 招式派生: a canvas with a right-side weapon / node library; node types and Boss move
// kinds are grouped inside the node tab. The Blueprint-style canvas holds
// trigger nodes (the NPC skill key, hero buttons), moves, intervals, 按键分支 (which button pressed next, after
// which pause, leads where) and the 加速 / 霸体 / 允许中断 / 武器属性 modifiers, which triggers and moves point at with
// straight arrows. Edits are saved per skin at once (NpcCombo); the game thread plays them (npc_retarget). The header
// (派生表) picks whose chain is edited: the applied look's (跟随当前外观, the default), the hero's or any look's.
namespace ComboMenu {
HWND CreatePage(HWND parent);
void Tick();
void Shutdown();
// Route text/navigation directly before the hub's dialog traversal.
bool OwnsTextMessage(const MSG& message);
// Consume recording input before hub navigation/dialog keys, including mouse buttons over other controls.
bool HandleRecordingMessage(const MSG& message);
// Worker thread, the command file's "combo ...": "menu" shows the page; "preview <look row> <move, 1-based>" holds
// that move's 招式预览 card up (top middle of the canvas) as if the mouse rested on it; "preview off" lets it go;
// "handsoff on|off": an open hub leaves the real mouse alone (tests in the background); "curve [close]" opens the
// first 加速 node's curve window (a test curve when the chain has none) and logs where it sits (COMBO_TEST lines).
bool HandleCommand(const char* arguments);
void SetLogger(void (*log)(const char*));
// Screen points (page client coordinates) for the offline preview's scripted mouse checks.
namespace Probe {
void RefreshData(); // explicit hidden-host driver for the same production refresh
void PickNodes(const std::vector<int>& nodes);
bool ClipboardShortcut(int key);
size_t ClipboardCount();
std::wstring TrimDurationText(int node);
std::wstring TrimSourceLabel(int node);
void PerformanceFixture(int count);
void PerformanceStats(uint64_t& measures,uint64_t& graphs,uint64_t& shapes);
void PerformanceHover(POINT p);
void PerformancePan(int dx);
POINT Header(int node);      // -1: the NPC key node
POINT Pin(int node, bool out);
POINT Field(int node);       // a trigger's kind field (opens the button picker); a 投技 node's grab field
POINT RouteOut(int node, int route);    // a branch row's output pin
POINT RouteField(int node, int route);  // a branch row's button field
POINT RoutePlus(int node, int route);   // a branch row's pause +
POINT RouteHoldPoint(int node,int route);
void RoutePicker(int node,int route,int part=0);
POINT RouteKeys(int node, int route);   // a 自定义组合键 / 自定义按键 row's keys field ({-1,-1}: the row has none)
POINT RouteMode(int node, int route);   // that row's mode field (in order / together; a 自定义按键 row's 吞掉 too)
POINT RouteAdd(int node);               // a branch's "+ 添加按键"
POINT ModOut(int node);      // a trigger's or a move's round pin (its arrows to modifiers start there)
POINT ModIn(int node);       // a modifier's round pin (arrows end there)
POINT Value(int node);       // a node's stepper value (a 加速's or a move's: click to type the multiplier / 派生点)
bool Editing();              // a 加速 multiplier or a move's 派生点 is being typed
POINT Plus(int node);        // a node's stepper + (interval, trigger reset, 加速, a move's 派生点)
POINT Minus(int node);       // its −
POINT ArrowHead(int node, int slot);       // a point inside the head of the node's arrow to its modifier in that slot ({-1,-1}: none)
COLORREF ArrowColor(int node, int slot);   // that arrow's colour
RECT NodeRect(int node);
void ZoomTo(int percent);
void MoveTo(int node, POINT p);  // puts a node's card over page point p (test setup)
int FreeTop();               // the canvas's first row under the floating tool bar (it wraps over the canvas on a narrow page)
void CenterOn(POINT p);      // pans so page point p sits in the middle of the canvas under the tool bar    // one of 50, 63 (62.5), 75, 88, 100, 125, 150
POINT Pill(int index);
RECT Canvas();
POINT Tool(int button);      // ComboMenu's Button order: Toggle, Interval, AddTrigger, Arrange, Frame, Remove, Clear, Branch, Speed, Weapon, Armor, Interrupt, Grab,
                             // BossSkill, File, SaveFile, SaveAs, WeaponMove, Damage, Group, Teleport, Invincible (21), Look (22, 变身), HeroLook (23, 变回主角),
                             // KeepLook (24, 保留变身), ForceHit (25, 强制受击), WeaponActions (26, 武器派生修改), Toast (27, 派生提示), Dummy (28, 测试木桩),
                             // AutoDodge (29, 按摇杆方向闪避), IgnoreGuard (30, 无视防御), KeepChain (31, 保持派生)
int Zoom();                  // percent
bool SearchOpen();
HWND SearchInput();
std::wstring SearchQuery();
void SearchAt(POINT at);
size_t Picked();              // nodes selected now (框选 / Ctrl + click; one node selected alone: 1)
bool PickedNode(int node);    // is that node among them
bool Empty(POINT p);          // bare canvas there
void Put(int node, int below);  // moves a node just under another (test setup)
void Show(int node);         // pans so the node sits at the canvas's top-left
size_t SearchRows();
int SearchSelected();            // the search box's picked row
POINT SearchRow(int row);        // an item row's middle in the search box ({-1,-1}: a group header, or scrolled away)
RECT SearchBox();                // the open search box
RECT PreviewCard();              // the 招式预览 card as last drawn (empty: none asked)
POINT WeaponRowOf(const char* id);  // the weapon list's row for a weapon id ({-1,-1}: not listed or scrolled away)
size_t WeaponRows();             // weapons listed
POINT Field2(int node);          // a node's second field (a BOSS 技能's / 武器派生's move, a 处决 trigger's weapon, a 组合键's keys)
POINT Field3(int node);          // a 组合键 / 自定义按键 trigger's third field: in order (and the gap) or together
POINT Field4(int node);          // a 组合键 / 自定义按键 trigger's 继续派生 field
POINT Header();                  // the 派生表 header (a click lists the chains to edit)
std::string EditedProfile();     // the profile whose chain the page edits now
POINT Toggle2(int node);         // a 自定义按键 trigger's 吞掉 checkbox
POINT Toggle(int node);          // a trigger's 启用 checkbox; a 加速's 曲线调节 checkbox (Field: its 编辑曲线…)
void RecordAt(int node,int route=-1); // hidden-test setup; does not focus a hidden page
std::vector<int> RecordingKeys(); // draft only, never a saved config
int RecordingNode();             // the 自定义按键 node recording keys (-1: none)
int RecordingRoute();            // its branch row recording keys (-1: none, or the node's own keys; -5: its 继续派生 key)
void ScrollWeapons(int pixels);  // turns the weapon list page (test setup)
// Hidden-window layout probes: no input injection, focus, mouse capture or file writes.
void Library(int tab, int category);
RECT LibraryBox();
RECT LibraryTabBox(int tab);
RECT LibraryCategoryBox(int category);
void LibraryTurn(int delta);
int LibraryPage();
int LibraryPageCount();
int ToolCategory(int button);
RECT ToolBox(int button);
// Direct trim probes: no focus/capture; end/wheel use normal saves (isolated test folder required).
void TrimPreviewAt(int node);
void BonePreviewAt(int node);
void BoneEditor(int node);
RECT TrimBarBox(int node);
POINT HoldValuePoint(int node);
bool ValueWheel(POINT p,int direction);
bool TrimBegin(int node,POINT p);
void TrimMove(POINT p);
bool TrimHoldToggle(int node,POINT p);
std::vector<int> TrimHoldPoints(int node);
void TrimEnd(POINT p);
int TrimValue(int node,bool end);
POINT Resize(int node);  // a 组合 frame's size handle
POINT Body(int node);    // an empty point in a 组合 frame's body
}
}
