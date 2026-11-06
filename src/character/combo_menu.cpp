#include "combo_menu.h"
#include "npc_combo.h"
#include "npc_retarget.h"
#include "debug_hub.h"
#include "motian_grab.h"
#include "test_dummy.h"
#include "grab_boss.h"
#include "combo_weapons.h"
#include "weapon_icons.h"
#include "ui_hotkeys.h"
#include "ui_i18n.h"
#include "ui_layout.h"
#include "ui_theme.h"
#include <objidl.h>
#include <gdiplus.h>
#include <windowsx.h>
#include <algorithm>
#include <cmath>
#include <cwchar>
#include <cwctype>
#include <map>
#include <string>
#include <utility>
#include <vector>

// The page is laid out like a Blueprint graph, in the hub's own colours and fonts: the skin's moves by kind on top
// (click to append, drag onto the canvas to place), then a canvas of nodes with exec pins and wires. Trigger nodes
// (the NPC skill key, or one of the hero's own buttons it replaces) start chains; every press plays the next move,
// a 按键分支 picks the next one by the button pressed (and the pause before it). 加速 / 霸体 / 武器属性 nodes point
// with a round pin at a trigger (its whole chain) or, 加速 / 霸体, at one move. Nodes are dragged freely, wires drawn from
// pin to pin, the canvas pans and zooms, Tab / right-click searches.
namespace ComboMenu {
namespace {
using NpcCombo::Kind;
using NpcCombo::Node;
using NpcCombo::NodeType;
using NpcCombo::Trigger;
constexpr wchar_t kClass[]=L"PBZComboPage_v3";
constexpr float kZooms[]={.5f,.625f,.75f,.875f,1.f,1.25f,1.5f};
// Node geometry in canvas units (96 dpi, zoom 1).
constexpr int kHeader=28,kRow=24,kPad=8,kPin=6,kGrid=16,kMajor=128;
enum class Button : int { Toggle, Interval, AddTrigger, Arrange, Frame, Remove, Clear, Branch, Speed, Weapon, Armor, Interrupt, Grab, BossSkill, File, SaveFile, SaveAs, WeaponMove, Damage,
                          Group, Teleport, Invincible, Look, HeroLook, KeepLook, ForceHit, WeaponActions, Toast, Dummy, AutoDodge, Count };
// The canvas tools, laid out right to left.
constexpr Button kTools[]={Button::SaveAs,Button::SaveFile,Button::File,Button::Clear,Button::Remove,Button::Frame,Button::Arrange,Button::Weapon,Button::Interrupt,Button::Invincible,Button::ForceHit,Button::Armor,Button::Damage,Button::Speed,Button::AddTrigger,Button::Branch,Button::Grab,Button::WeaponActions,Button::WeaponMove,Button::Teleport,Button::HeroLook,Button::KeepLook,Button::Look,Button::BossSkill,Button::Group,Button::AutoDodge,Button::Interval};
enum class Part : int {
    None, Button, Pill, Tab, StripLeft, StripRight, Canvas, Search, WeaponRow,
    Node, NodeIn, NodeOut, NodeMinus, NodePlus, NodeValue, NodeField, NodeField2, NodeField3, NodeField4, NodeToggle, NodeToggle2, GroupResize, NodeRemove,
    RouteField, RouteMinus, RoutePlus, RouteValue, RouteRemove, RouteAdd, RouteOut, ModOut, ModIn, RouteKeys, RouteMode, Header
};
// sub: a branch's route row (-1 otherwise).
struct Hit { Part part=Part::None; int index=-1; int sub=-1; bool operator==(const Hit& o) const { return part==o.part&&index==o.index&&sub==o.sub; } };
struct Pill { RECT rect{}; std::string ability; Kind kind=Kind::Other; };
struct Tab { int kind=-1; RECT rect{}; size_t count=0; };  // kind -1: every move
// A node's parts on screen. in: the exec input (a trigger: where its round pin sits); mod_out: a trigger's or a
// move's round pin, where its arrows to 加速 / 霸体 / 武器属性 start; mod_in: a modifier's round pin, where they end;
// routes: a branch's rows, each with its own exec output.
// A 自定义组合键 / 自定义按键 row has a second line: its keys and how they are pressed (has_keys).
struct RouteShape { RECT field{},minus{},value{},plus{},remove{},keys{},mode{}; POINT out{}; bool has_keys=false; };
struct Shape {
    RECT rect{},header{},remove{},minus{},value{},plus{},field{},field2{},field3{},field4{},toggle{},toggle2{},add{},resize{}; POINT in{},out{},mod_out{},mod_in{};
    bool has_in=false,has_out=false,has_mod_out=false,has_mod_in=false; std::vector<RouteShape> routes;
};
// Arrow: from a node's round pin to a modifier; ArrowBack: from a modifier's pin to the node that points at it.
enum class Drag { None, Pending, Node, Pan, Wire, WireBack, Arrow, ArrowBack, Pill, WeaponRow, Resize, Box };
struct DragState { Drag mode=Drag::None; Hit hit; POINT start{},last{}; bool moved=false,right=false; int index=-1,sub=-1; };
// The search box (Tab, right-click, or a wire dropped on empty canvas): pick a node to add where it opened. In pick
// mode (a trigger's or a route's button field) it lists the buttons and sets that field; on a 投技 node's field it
// lists the grabs of the look's boss (GrabMove).
// A BOSS 技能 node's fields list the looks (BossLook) and that look's moves (BossMove); a 变身 node's field lists every
// look (BossLook too); the 文件 button lists the
// .combo files (File) and 另存为 takes a name (SaveAs).
// A 武器派生 node's fields list the weapons (WeaponPick) and that weapon's abilities (WeaponSkill); a 处决 trigger's
// weapon field lists the weapons too, 任意武器 first.
enum class ItemKind { Move, Interval, AutoDodge, Trigger, Branch, Speed, Weapon, Armor, Interrupt, Grab, Button, GrabMove, BossSkill, BossLook, BossMove, File, SaveAs,
                      WeaponMove, Damage, WeaponPick, WeaponSkill, Group, Teleport, TeleportDist, TeleportLift, ComboKey, ComboMode, RawKey, Invincible,
                      Look, HeroLook, KeepLook, ForceHit, HitKind, WeaponActions, ThenKey, Chain };
struct Item { ItemKind kind=ItemKind::Move; std::wstring label,group,keys; std::string ability; int trigger=0; Kind move=Kind::Other; };
struct SearchRow { bool header=false; size_t item=0; std::wstring text; };
struct Search {
    bool open=false; RECT rect{},edit{},list{}; float x=0,y=0; int from=-1,into=-1,from_out=0,pick_node=-1,pick_route=-1,mod_from=-1,mode=0; // mode 1 files, 2 save-as
    int pick_part=0;  // a branch row's field (pick_route >= 0): 0 its button, 1 its keys, 2 how they are pressed
    std::wstring query; std::vector<Item> items; std::vector<SearchRow> rows; int selected=-1,scroll=0,hot=-1;
};

HWND window_=nullptr;
int32_t skin_=-1;
// 派生表: whose chain the page edits. kFollowGame: the one the game runs now (the applied look's, or the hero's);
// else a look's row (kProfiles) or -1 the hero's, picked on the header, whatever the game has on.
constexpr int32_t kFollowGame=-2;
int32_t pinned_=kFollowGame,game_chain_=-1;
NpcRetarget::SkinMoves moves_;
NpcCombo::Config config_;
NpcCombo::Run run_;
std::vector<Pill> pills_;
std::vector<Tab> tabs_;
RECT buttons_[static_cast<int>(Button::Count)]{};
RECT header_{},tabs_title_{},strip_{},strip_area_{},strip_left_{},strip_right_{},canvas_{},footer_{};
int category_=-1,strip_scroll_=0;
int height_=0,selected_=-1,zoom_index_=4;
// Several nodes selected at once (Ctrl / Shift + drag on bare canvas: 框选; Ctrl + click a node adds or drops it):
// selected_ is one of them. Empty: selected_ alone. They are dragged together and removed together.
std::vector<int> picked_;
float pan_x_=0,pan_y_=0;  // canvas origin offset in screen pixels
bool framed_=false;
bool dummy_present_=false;  // 测试木桩 stands (TestDummy snapshot, read every Tick)
std::wstring dummy_status_;  // its last status, shown once as a notice
Hit hot_,pressed_;
DragState drag_;
Search search_;
// The 加速 multiplier being typed (node -1: none); fresh: the first key typed replaces the text.
struct Edit { int node=-1; std::wstring text; bool fresh=true; } edit_;
POINT cursor_{-1,-1};
std::wstring hint_text_;
UiI18n::Language language_=UiI18n::Language::SimplifiedChinese;
ULONGLONG next_=0;
uint64_t run_serial_=0,weapon_serial_=0;
UINT font_dpi_=0;
std::map<int,HFONT> scaled_;

std::wstring Tr(const wchar_t* text) { return UiI18n::Translate(text?text:L""); }
UiLayout::Metrics M() { return UiLayout::For(window_); }
HFONT Font(UiTheme::Font role) { return UiTheme::GetFont(role,window_); }
float Zoom() { return kZooms[zoom_index_]; }
float Dpi() { return M().S(1000)/1000.f; }
float Scale() { return Zoom()*Dpi(); }
void DropFonts() { for (auto& [key,font]:scaled_) DeleteObject(font); scaled_.clear(); }
// The hub's fonts at the canvas zoom.
HFONT Scaled(UiTheme::Font role) {
    const UINT dpi=static_cast<UINT>(M().S(96));
    if (dpi!=font_dpi_) { DropFonts(); font_dpi_=dpi; }
    const int key=static_cast<int>(role)*100+zoom_index_;
    const auto found=scaled_.find(key);
    if (found!=scaled_.end()) return found->second;
    LOGFONTW logfont{}; GetObjectW(Font(role),sizeof(logfont),&logfont);
    logfont.lfHeight=static_cast<LONG>(std::lround(logfont.lfHeight*Zoom())); if (!logfont.lfHeight) logfont.lfHeight=-1;
    const HFONT font=CreateFontIndirectW(&logfont);
    scaled_[key]=font?font:Font(role);
    return scaled_[key];
}
Gdiplus::Color Argb(COLORREF color,BYTE alpha=255) { return Gdiplus::Color(alpha,GetRValue(color),GetGValue(color),GetBValue(color)); }
void RoundedPath(Gdiplus::GraphicsPath& path,float x,float y,float w,float h,float r,bool top_only=false) {
    const float d=(std::min)(r*2,(std::min)(w,h));
    if (d<=0) { path.AddRectangle(Gdiplus::RectF(x,y,w,h)); return; }
    path.AddArc(x,y,d,d,180,90); path.AddArc(x+w-d,y,d,d,270,90);
    if (top_only) { path.AddLine(x+w,y+d/2,x+w,y+h); path.AddLine(x+w,y+h,x,y+h); }
    else { path.AddArc(x+w-d,y+h-d,d,d,0,90); path.AddArc(x,y+h-d,d,d,90,90); }
    path.CloseFigure();
}
void Rounded(Gdiplus::Graphics& graphics,const RECT& rect,float radius,COLORREF fill,BYTE fill_alpha,COLORREF edge,BYTE edge_alpha,float width=1.f,bool top_only=false) {
    const float x=static_cast<float>(rect.left)+.5f,y=static_cast<float>(rect.top)+.5f;
    const float w=static_cast<float>(rect.right-rect.left)-1.f,h=static_cast<float>(rect.bottom-rect.top)-1.f;
    if (w<=0||h<=0) return;
    Gdiplus::GraphicsPath path; RoundedPath(path,x,y,w,h,radius,top_only);
    if (fill_alpha) { Gdiplus::SolidBrush brush(Argb(fill,fill_alpha)); graphics.FillPath(&brush,&path); }
    if (edge_alpha) { Gdiplus::Pen pen(Argb(edge,edge_alpha),width); graphics.DrawPath(&pen,&path); }
}
void Label(HDC dc,const std::wstring& text,RECT rect,HFONT font,COLORREF color,UINT flags) {
    const HGDIOBJ old=SelectObject(dc,font); SetTextColor(dc,color);
    DrawTextW(dc,text.c_str(),-1,&rect,flags|DT_NOPREFIX); SelectObject(dc,old);
}
SIZE Extent(HDC dc,HFONT font,const std::wstring& text,int wrap=0) {
    const HGDIOBJ old=SelectObject(dc,font);
    RECT area{0,0,wrap>0?wrap:100000,0};
    DrawTextW(dc,text.empty()?L" ":text.c_str(),-1,&area,DT_CALCRECT|DT_NOPREFIX|(wrap>0?DT_WORDBREAK|DT_EDITCONTROL:DT_SINGLELINE));
    TEXTMETRICW metrics{}; GetTextMetricsW(dc,&metrics); SelectObject(dc,old);
    return {area.right,(std::max)(static_cast<LONG>(metrics.tmHeight),area.bottom)};
}
// Canvas units a text needs at zoom 1.
int Units(HDC dc,UiTheme::Font role,const std::wstring& text) { return static_cast<int>(std::ceil(Extent(dc,Font(role),text).cx/Dpi())); }
COLORREF Paper() { return UiTheme::Mix(UiTheme::Base(),UiTheme::Panel(),72); }
// One colour per kind, mixed from the theme so it stays readable in both themes.
COLORREF KindColor(Kind kind) {
    static constexpr COLORREF kHues[]={RGB(96,160,210),RGB(214,108,72),RGB(98,184,128),RGB(150,190,90),RGB(200,150,60),RGB(180,120,200),RGB(220,80,90),RGB(90,170,170),RGB(150,150,150)};
    const int index=std::clamp(static_cast<int>(kind),0,static_cast<int>(Kind::Count)-1);
    return UiTheme::Mix(kHues[index],UiTheme::Text(),15);
}
COLORREF TriggerColor(int trigger) {
    if (trigger==static_cast<int>(Trigger::NpcKey)) return UiTheme::Accent();
    return NpcCombo::IsDodge(static_cast<Trigger>(trigger))?UiTheme::Mix(UiTheme::Danger(),UiTheme::Accent(),25):UiTheme::Danger();
}
Kind KindOf(const std::string& ability) {
    if (std::find(moves_.dodges.begin(),moves_.dodges.end(),ability)!=moves_.dodges.end()) return Kind::Dodge;
    return NpcCombo::Classify(ability);
}
std::wstring MoveText(const std::string& ability) { return ability.empty()?Tr(L"（无效招式）"):NpcRetarget::MoveLabel(ability); }
std::wstring Seconds(double s) { wchar_t text[32]{}; _snwprintf_s(text,_countof(text),_TRUNCATE,L"%.2f s",s); return text; }
// A chain to edit: an NPC look's, or the hero's own (skin_ -1, profile NpcCombo::kHeroProfile).
bool HasSkin() { return !moves_.profile.empty(); }
// The header: whose chain this is and when the game runs it; a click lists the others.
std::wstring HeaderText() {
    if (!HasSkin()) return Tr(L"未套用 NPC 外观：先在 NPC 页套用一个外观，这里会列出它的全部招式。");
    const wchar_t* when=pinned_==kFollowGame?L"（跟随当前外观）":skin_==game_chain_?L"（当前外观）":
                        skin_==-1?L"（没有套用 NPC 外观时生效）":L"（游戏中套用这个外观时生效）";
    return Tr(L"派生表：")+Tr(moves_.name.c_str())+Tr(when)+L"  ▾";
}
uint64_t seen_generation_=0,files_serial_=0;ULONGLONG hero_check_=0;NpcCombo::Files files_;std::wstring file_status_;
bool MoveOfSkin(const std::string& ability) {
    return std::find(moves_.skills.begin(),moves_.skills.end(),ability)!=moves_.skills.end()||
           std::find(moves_.dodges.begin(),moves_.dodges.end(),ability)!=moves_.dodges.end();
}
// A file status from NpcCombo: a fixed Chinese prefix up to the first full-width colon, then the file / reason.
std::wstring StatusText(const std::wstring& status) {
    const size_t colon=status.find(L'：');
    return colon==std::wstring::npos?Tr(status.c_str()):Tr(status.substr(0,colon+1).c_str())+status.substr(colon+1);
}
void Save() { if (HasSkin()) { NpcCombo::Save(moves_.profile,config_); seen_generation_=NpcCombo::Generation(); } }
// The looks a BOSS 技能 may take: every NPC look with moves (id, name), read once.
const std::vector<std::pair<std::string,std::wstring>>& BossLooks() {
    static std::vector<std::pair<std::string,std::wstring>> looks;
    if (looks.empty())
        for (const NpcRetarget::Entry& e:NpcRetarget::GetSnapshot().entries) { NpcRetarget::SkinMoves m; if (NpcRetarget::GetSkinMoves(e.index,m)&&!m.skills.empty()) looks.emplace_back(e.id,e.name); }
    return looks;
}
// The looks a 变身 may take: every NPC look (id, name), read once.
const std::vector<std::pair<std::string,std::wstring>>& AllLooks() {
    static std::vector<std::pair<std::string,std::wstring>> looks;
    if (looks.empty()) for (const NpcRetarget::Entry& e:NpcRetarget::GetSnapshot().entries) looks.emplace_back(e.id,e.name);
    return looks;
}
std::wstring BossLookName(const std::string& id) {
    for (const auto& [look,name]:AllLooks()) if (look==id) return name;
    return std::wstring(id.begin(),id.end());
}
int32_t BossLookRow(const std::string& id) { for (const NpcRetarget::Entry& e:NpcRetarget::GetSnapshot().entries) if (e.id==id) return e.index; return -1; }
COLORREF BossColor() { return UiTheme::HighContrast()?UiTheme::AccentBlue():UiTheme::Mix(RGB(206,92,64),UiTheme::Text(),12); }
COLORREF WeaponColor() { return UiTheme::HighContrast()?UiTheme::AccentBlue():UiTheme::Mix(RGB(86,150,205),UiTheme::Text(),14); }
// The weapon list on the canvas's right: one row per weapon (main weapons, then 杀器 under a header), scrolled with
// the wheel; a row is clicked (a 武器派生 node after the selected one) or dragged onto the canvas / a node.
struct WeaponRow { RECT rect{}; std::string id; bool shaqi=false,header=false; std::wstring name; int image=-1; };
std::vector<WeaponRow> weapon_rows_; RECT weapons_{},weapons_title_{}; int weapon_scroll_=0;
bool IsShaqiId(const std::string& id) { return ComboWeapons::IsShaqi(id); }
std::wstring WeaponName(const std::string& id) {
    const std::wstring wide(id.begin(),id.end());
    const WeaponIcons::Info info=WeaponIcons::Find(wide.c_str(),IsShaqiId(id));
    return info.name?std::wstring(info.name):wide;
}
// The weapons offered: those with abilities, one per name (the game's variants of a weapon share it), main first.
std::vector<std::pair<std::string,bool>> ListedWeapons() {
    std::vector<std::pair<std::string,bool>> out; std::vector<std::wstring> names;
    for (const bool shaqi:{false,true})
        for (const ComboWeapons::Weapon& w:ComboWeaponsData::kWeapons) {
            if (w.shaqi!=shaqi||!w.count) continue;
            const std::wstring name=WeaponName(w.id);
            if (std::find(names.begin(),names.end(),name)!=names.end()) continue;
            names.push_back(name); out.push_back({w.id,w.shaqi});
        }
    return out;
}
bool HasExecution(const std::string& id) {
    const ComboWeapons::Weapon* w=ComboWeapons::Find(id); if (!w) return false;
    for (size_t i=0;i<w->count;++i) if (w->moves[i].kind==ComboWeapons::kExecution) return true;
    return false;
}
std::wstring WeaponKindLabel(int kind) { return Tr(kind==ComboWeapons::kLight?L"轻攻击":kind==ComboWeapons::kHeavy?L"重攻击":kind==ComboWeapons::kExecution?L"处决":L"技能"); }
// A weapon ability's label: a combo stage's presses first ("轻轻重 · ...", English "LLH · ..."), then the ability's name.
std::wstring MoveText(const std::string& ability);
std::wstring WeaponMoveText(const std::string& weapon,const std::string& ability) {
    const ComboWeapons::Move* move=ComboWeapons::MoveOf(weapon,ability);
    const std::wstring name=MoveText(ability);
    if (!move||!move->stage[0]) return name;
    std::wstring presses=ComboWeapons::StagePresses(move->stage);
    if (UiI18n::GetLanguage()==UiI18n::Language::English) for (wchar_t& c:presses) c=c==L'重'?L'H':L'L';
    return presses+L" · "+name;
}
std::wstring DamageText(int percent) { wchar_t text[24]{}; _snwprintf_s(text,_countof(text),_TRUNCATE,L"%d%%",percent); return text; }
// A 处决 / 简易处决 trigger: whose executions it takes (any weapon or one).
bool ExecutionTrigger(const Node& node) { return node.type==NodeType::Trigger&&NpcCombo::WeaponTrigger(node.trigger); }
bool WeaponActionsNode(const Node& node) { return node.type==NodeType::WeaponActions; }
// A 自定义组合键 / 自定义按键 trigger: its keys (field2) and how they are pressed (field3); 自定义按键 also whether its
// keys' game actions are swallowed (toggle2).
bool KeysTrigger(const Node& node) { return NpcCombo::IsComboTrigger(node)||NpcCombo::IsKeysTrigger(node); }
bool RawTrigger(const Node& node) { return NpcCombo::IsKeysTrigger(node); }
// 自定义按键 recording: keys pressed on the page (keyboard, mouse middle / side buttons) or on a pad join the node (or
// its branch row `route`, -1: the node's own keys).
// route kRecordThen: a 自定义按键 trigger's 继续派生 key (one key, then the recording ends).
constexpr int kRecordThen=-5;
struct Recording { int node=-1; int route=-1; unsigned pad_was=0; bool pad_seeded=false; };
Recording record_;
// A pad button by the pad in use (Xbox or PlayStation names), or both for the list.
std::wstring PadLabel(int code,bool ps) {
    static const wchar_t* const kXbox[NpcCombo::kPadBits+NpcCombo::kStickWays]={L"方向键上",L"方向键下",L"方向键左",L"方向键右",L"Start",L"Back",L"L3",L"R3",
                                                       L"LB",L"RB",L"A",L"B",L"X",L"Y",L"LT",L"RT",L"左摇杆上推",L"左摇杆下推",L"左摇杆左推",L"左摇杆右推"};
    static const wchar_t* const kPs[NpcCombo::kPadBits+NpcCombo::kStickWays]={L"方向键上",L"方向键下",L"方向键左",L"方向键右",L"Options",L"Create",L"L3",L"R3",
                                                     L"L1",L"R1",L"叉",L"圈",L"方块",L"三角",L"L2",L"R2",L"左摇杆上推",L"左摇杆下推",L"左摇杆左推",L"左摇杆右推"};
    return NpcCombo::IsPadKey(code)?Tr((ps?kPs:kXbox)[code-NpcCombo::kPadKey]):std::wstring();
}
std::wstring PadBothLabel(int code) {
    const std::wstring xbox=PadLabel(code,false),ps=PadLabel(code,true);
    return xbox==ps?xbox:xbox+L" / "+ps;
}
std::wstring KeyboardLabel(int vk) {
    switch (vk) {
    case VK_MBUTTON: return Tr(L"鼠标中键");
    case VK_XBUTTON1: return Tr(L"鼠标侧键 1");
    case VK_XBUTTON2: return Tr(L"鼠标侧键 2");
    default: break;
    }
    UINT scan=MapVirtualKeyW(static_cast<UINT>(vk),MAPVK_VK_TO_VSC);
    switch (vk) {
    case VK_PRIOR: case VK_NEXT: case VK_END: case VK_HOME: case VK_LEFT: case VK_UP: case VK_RIGHT: case VK_DOWN: case VK_INSERT: case VK_DELETE:
    case VK_DIVIDE: case VK_NUMLOCK: case VK_RCONTROL: case VK_RMENU: case VK_LWIN: case VK_RWIN: case VK_APPS: scan|=0x100; break;
    default: break;
    }
    wchar_t name[64]{};
    if (scan&&GetKeyNameTextW(static_cast<LONG>(scan<<16),name,static_cast<int>(_countof(name)))>0) return name;
    wchar_t text[24]{}; _snwprintf_s(text,_countof(text),_TRUNCATE,L"VK 0x%02X",vk); return text;
}
std::wstring RawKeyLabel(int code) {
    return NpcCombo::IsPadKey(code)?PadLabel(code,UiHotkeys::ActivePad()==UiHotkeys::Device::PlayStation):KeyboardLabel(code);
}
int Px(int units);
std::wstring KeysText(const std::vector<int>& keys,bool chord,bool raw) {
    std::wstring out;
    if (raw) {
        for (const int key:keys) out+=(out.empty()?L"":chord?L" + ":L" → ")+RawKeyLabel(key);
        return out.empty()?Tr(L"选择或录制按键（1 到 6 个）"):out;
    }
    for (const int key:keys) out+=(out.empty()?L"":chord?L" + ":L" → ")+Tr(NpcCombo::ComboKeyLabel(key));
    return out.empty()?Tr(L"选择按键（至少 2 个）"):keys.size()<NpcCombo::kComboKeysMin?out+L" · "+Tr(L"至少 2 个"):out;
}
std::wstring KeysText(const Node& node) { return KeysText(node.keys,node.chord,RawTrigger(node)); }
// A 自定义组合键 / 自定义按键 trigger's 继续派生 field (field4): what takes its chain on after the first move.
std::wstring ButtonLabel(int button);
std::wstring ThenText(const Node& node,bool recording) {
    if (recording) return Tr(L"继续派生：按下一个键…");
    if (!NpcCombo::HasThen(node)) return Tr(RawTrigger(node)?L"继续派生：再按原按键":L"继续派生：再按原组合键");
    const int key=NpcCombo::ThenRawKey(node);
    return Tr(L"继续派生：")+(key>=0?RawKeyLabel(key):ButtonLabel(node.then));
}
// Keys as icons: keyboard and mouse keys as keycaps, pad buttons as the game's prompt icons for the pad in use, the
// left stick's ways as its L3 icon with an arrow, a 自定义组合键's hero buttons as keycaps with their names; "+"
// (together) or "→" (in order) between two. Heights and widths in pixels.
constexpr int kKeyIcon=18;  // icon height in canvas units (a field is kRow-4 high)
bool PsPad() { return UiHotkeys::ActivePad()==UiHotkeys::Device::PlayStation; }
bool MouseKey(int code) { return code==VK_MBUTTON||code==VK_XBUTTON1||code==VK_XBUTTON2; }
UiHotkeys::Chord KeyChord(int code) {
    UiHotkeys::Chord chord;
    if (NpcCombo::IsPadKey(code)) { chord.device=PsPad()?UiHotkeys::Device::PlayStation:UiHotkeys::Device::Xbox; chord.buttons=1u<<(code-NpcCombo::kPadKey); }
    else { chord.device=UiHotkeys::Device::Keyboard; chord.key=static_cast<unsigned>(code); }
    return chord;
}
int KeyIconWidth(HDC dc,int code,bool raw,int h) {
    if (!raw) return UiHotkeys::KeyWidth(dc,h,Tr(NpcCombo::ComboKeyLabel(code)).c_str());
    if (NpcCombo::IsStickKey(code)) return UiHotkeys::StickWidth(h,PsPad());
    if (MouseKey(code)) return UiHotkeys::KeyWidth(dc,h,KeyboardLabel(code).c_str());
    return (std::max)(h,UiHotkeys::Width(dc,h,KeyChord(code))-h/5);  // Width counts the gap after the glyph
}
int DrawKeyIcon(HDC dc,int x,int y,int code,bool raw,int h) {
    const int width=KeyIconWidth(dc,code,raw,h);
    if (!raw) UiHotkeys::DrawKey(dc,x,y,h,Tr(NpcCombo::ComboKeyLabel(code)).c_str());
    else if (NpcCombo::IsStickKey(code)) UiHotkeys::DrawStick(dc,x,y,h,code-NpcCombo::kStickKey,PsPad());
    else if (MouseKey(code)) UiHotkeys::DrawKey(dc,x,y,h,KeyboardLabel(code).c_str());
    else UiHotkeys::Draw(dc,x,y,h,KeyChord(code),x+width+h);
    return x+width;
}
int KeySeparator(int h) { return h*3/4; }
int KeyIconsWidth(HDC dc,const std::vector<int>& keys,bool raw,int h) {
    int width=0;
    for (size_t k=0;k<keys.size();++k) width+=(k?KeySeparator(h):0)+KeyIconWidth(dc,keys[k],raw,h);
    return width;
}
// The keys' icons from rect.left, centred in its height; what does not fit before `limit` is "…". Returns the x after.
int DrawKeyIcons(HDC dc,const RECT& rect,const std::vector<int>& keys,bool chord,bool raw,COLORREF muted,int limit) {
    const int h=(std::min)(Px(kKeyIcon),static_cast<int>(rect.bottom-rect.top)),y=(rect.top+rect.bottom-h)/2;
    int x=rect.left;
    for (size_t k=0;k<keys.size();++k) {
        const int width=KeyIconWidth(dc,keys[k],raw,h),sep=k?KeySeparator(h):0;
        if (x+sep+width>limit) { RECT more{x,rect.top,(std::min)(limit+Px(14),x+Px(14)),rect.bottom}; Label(dc,L"…",more,Scaled(UiTheme::Font::Small),muted,DT_SINGLELINE|DT_VCENTER|DT_LEFT); return more.right; }
        if (sep) { RECT between{x,rect.top,x+sep,rect.bottom}; Label(dc,chord?L"+":L"→",between,Scaled(UiTheme::Font::Small),muted,DT_SINGLELINE|DT_VCENTER|DT_CENTER); x+=sep; }
        x=DrawKeyIcon(dc,x,y,keys[k],raw,h);
    }
    return x;
}
// A keys field (the node's or a branch row's): icons, or the hint while there are none (or too few to work), and the
// "▾"; while recording the "录制中" note first.
void KeysField(HDC dc,const RECT& inner,const std::vector<int>& keys,bool chord,bool raw,bool recording,bool set,UiTheme::Font font,COLORREF text,COLORREF muted) {
    const size_t least=raw?1:NpcCombo::kComboKeysMin;
    if (keys.empty()||keys.size()<least) {
        const std::wstring hint=recording?Tr(L"录制中：按键盘或手柄上的键（Esc 结束）")+(keys.empty()?L"":L" · "+KeysText(keys,chord,raw)):KeysText(keys,chord,raw);
        Label(dc,hint+L"  ▾",inner,Scaled(font),set?text:muted,DT_SINGLELINE|DT_VCENTER|DT_LEFT|DT_END_ELLIPSIS); return;
    }
    const int arrow=Px(14);RECT area=inner;
    if (recording) {
        const std::wstring note=Tr(L"录制中")+L" · ";RECT left=inner;
        Label(dc,note,left,Scaled(font),UiTheme::Accent(),DT_SINGLELINE|DT_VCENTER|DT_LEFT);
        area.left+=static_cast<int>(std::lround(Units(dc,font,note)*Scale()));
    }
    DrawKeyIcons(dc,area,keys,chord,raw,muted,inner.right-arrow);
    RECT tail{inner.right-arrow,inner.top,inner.right,inner.bottom};
    Label(dc,L"▾",tail,Scaled(font),set?text:muted,DT_SINGLELINE|DT_VCENTER|DT_RIGHT);
}
// Canvas units the keys' icons take (at kKeyIcon), with room for the "▾" (and the 录制中 note while recording).
int KeyIconsUnits(HDC dc,const std::vector<int>& keys,bool raw,bool recording,UiTheme::Font font) {
    const size_t least=raw?1:NpcCombo::kComboKeysMin;
    if (keys.empty()||keys.size()<least) return 0;
    const int px=KeyIconsWidth(dc,keys,raw,Px(kKeyIcon));
    return static_cast<int>(std::ceil(px/Scale()))+18+(recording?Units(dc,font,Tr(L"录制中")+L" · "):0);
}
std::wstring KeysModeText(size_t count,bool chord,int window,bool raw) {
    if (raw&&count<=1) return Tr(L"单键：按下即放");
    if (chord) return Tr(L"一起按 · 150 毫秒内");
    wchar_t text[96]{}; _snwprintf_s(text,_countof(text),_TRUNCATE,Tr(L"按顺序按 · 间隔 %d 毫秒内").c_str(),NpcCombo::ClampKeyWindow(window));
    return text;
}
std::wstring KeysModeText(const Node& node) { return KeysModeText(node.keys.size(),node.chord,node.window,RawTrigger(node)); }
// A branch row's keys and mode (a 自定义按键 row's mode also says whether its keys' game actions are swallowed).
bool RawRoute(const NpcCombo::Route& route) { return route.button==static_cast<int>(Trigger::Keys); }
std::wstring RouteModeText(const NpcCombo::Route& route) {
    std::wstring text=KeysModeText(route.keys.size(),route.chord,route.window,RawRoute(route));
    if (RawRoute(route)) text+=L" · "+Tr(route.swallow?L"吞掉原动作":L"不吞掉");
    return text;
}
// The keys a picker or the recording edits: a 自定义组合键 / 自定义按键 trigger's own (route -2: its keys, -3: how they
// are pressed) or a branch row's of those (route >= 0; part 1: its keys, 2: how). keys null: none of these.
struct KeySpec { std::vector<int>* keys=nullptr; bool* chord=nullptr; int* window=nullptr; bool* swallow=nullptr; bool raw=false,row=false; int what=0; };
KeySpec PickedKeys(int node,int route,int part) {
    KeySpec k;
    if (node<0||static_cast<size_t>(node)>=config_.chain.size()) return k;
    Node& n=config_.chain[static_cast<size_t>(node)];
    if (KeysTrigger(n)&&(route==-2||route==-3)) {
        k.keys=&n.keys; k.chord=&n.chord; k.window=&n.window; k.swallow=&n.swallow; k.raw=RawTrigger(n); k.what=route==-2?1:2; return k;
    }
    if (n.type==NodeType::Branch&&route>=0&&static_cast<size_t>(route)<n.routes.size()&&(part==1||part==2)&&NpcCombo::KeysRoute(n.routes[static_cast<size_t>(route)])) {
        NpcCombo::Route& r=n.routes[static_cast<size_t>(route)];
        k.keys=&r.keys; k.chord=&r.chord; k.window=&r.window; k.swallow=&r.swallow; k.raw=RawRoute(r); k.row=true; k.what=part;
    }
    return k;
}
constexpr int kKeyWindowPresets[]={200,300,400,500,700,1000,1500};
std::wstring ExecutionWeaponText(const Node& node) { return Tr(L"武器：")+(node.weapon.empty()?Tr(L"任意武器"):WeaponName(node.weapon)); }
// A 武器派生修改's weapon field.
std::wstring ActionsWeaponText(const Node& node) { return node.weapon.empty()?Tr(L"选择武器"):Tr(L"武器：")+WeaponName(node.weapon); }
void Invalidate() { if (window_) InvalidateRect(window_,nullptr,FALSE); }
void Layout();
void ScrollStrip(int delta);
const Node* At(int index) { return index>=0&&static_cast<size_t>(index)<config_.chain.size()?&config_.chain[static_cast<size_t>(index)]:nullptr; }
bool IsKey(int index) { const Node* n=At(index); return n&&n->type==NodeType::Trigger&&n->trigger==static_cast<int>(Trigger::NpcKey); }
// 预输入: the chain's fixed setting card (never removed, no pins).
bool IsPreInputAt(int index) { const Node* n=At(index); return n&&NpcCombo::IsPreInput(*n); }
std::wstring PreInputText(int ms) { return ms>0?Seconds(ms/1000.0):Tr(L"不清除"); }
// ---- 组合 frames ------------------------------------------------------------------------------------
// A 组合 is a frame on the canvas: the nodes in it (Node::group) play one after another as one move of the chain. Its
// own pins sit on its header; the nodes in it wire only to each other (their order), never out of the frame.
bool IsFrame(int index) { return NpcCombo::IsGroup(config_,index); }
int FrameOf(int index) { const Node* n=At(index); return n?n->group:-1; }
// Wired into a chain: reached by a trigger's wires, or sitting in a 组合 that is.
bool Wired(size_t index) {
    int at=static_cast<int>(index);
    for (int depth=0;at>=0&&depth<=NpcCombo::kGroupDepthMax+1;++depth) {
        if (NpcCombo::TriggerOf(config_,static_cast<size_t>(at))>=0) return true;
        at=FrameOf(at);
    }
    return false;
}
// May an exec wire join these two (both outside every frame, or both straight in the same one)?
bool SameFrame(int a,int b) { return FrameOf(a)==FrameOf(b); }
// May `node` (not yet on the canvas) go into frame `group`?
bool Fits(const Node& node,int group) {
    if (group<0) return true;
    return IsFrame(group)&&NpcCombo::Groupable(node)&&NpcCombo::GroupDepth(config_,group)+1+(node.type==NodeType::Group?1:0)<=NpcCombo::kGroupDepthMax;
}
std::wstring CardTitle(size_t index);
SIZE NodeSize(HDC dc,size_t index);
// The innermost frame holding canvas point (x, y) that node `index` may go into (-1: none). `index` -1: a new node of
// type `type`.
int FrameAt(HDC dc,float x,float y,int index,NodeType type=NodeType::Skill) {
    int best=-1,depth=-1;
    for (size_t g=0;g<config_.chain.size();++g) {
        if (!IsFrame(static_cast<int>(g))||static_cast<int>(g)==index) continue;
        const Node& f=config_.chain[g]; const SIZE s=NodeSize(dc,g);
        if (x<f.x||y<f.y||x>f.x+s.cx||y>f.y+s.cy) continue;
        Node probe; probe.type=type;
        const bool fits=index>=0?NpcCombo::CanJoin(config_,index,static_cast<int>(g)):Fits(probe,static_cast<int>(g));
        const int d=NpcCombo::GroupDepth(config_,static_cast<int>(g));
        if (fits&&d>depth) { best=static_cast<int>(g); depth=d; }
    }
    return best;
}
// The frames around a node grow to hold it (with a margin; never smaller).
void FitFrames(HDC dc,int index) {
    for (int child=index,guard=0;At(child)&&FrameOf(child)>=0&&guard<=NpcCombo::kGroupDepthMax;child=FrameOf(child),++guard) {
        Node& f=config_.chain[static_cast<size_t>(FrameOf(child))]; const Node& c=config_.chain[static_cast<size_t>(child)];
        const SIZE cs=NodeSize(dc,static_cast<size_t>(child)),fs=NodeSize(dc,static_cast<size_t>(FrameOf(child)));
        const int top_room=kHeader+kRow+8,margin=16;
        const int left=(std::min)(f.x,c.x-margin),top=(std::min)(f.y,c.y-top_room);
        const int right=(std::max)(f.x+static_cast<int>(fs.cx),c.x+static_cast<int>(cs.cx)+margin),bottom=(std::max)(f.y+static_cast<int>(fs.cy),c.y+static_cast<int>(cs.cy)+margin);
        f.x=NpcCombo::ClampPosition(left); f.y=NpcCombo::ClampPosition(top);
        f.w=NpcCombo::ClampGroupW(right-f.x); f.h=NpcCombo::ClampGroupH(bottom-f.y);
    }
}
// Exec wires into and out of node `index` that cross a frame's edge are cut; true when one was.
bool CutCrossing(int index) {
    bool cut=false;
    Node& node=config_.chain[static_cast<size_t>(index)];
    for (size_t o=0;o<NpcCombo::OutCount(node);++o) { const int to=NpcCombo::OutOf(node,o); if (to>=0&&!SameFrame(index,to)) { NpcCombo::SetOut(node,o,-1); cut=true; } }
    for (size_t i=0;i<config_.chain.size();++i) {
        Node& from=config_.chain[i];
        for (size_t o=0;o<NpcCombo::OutCount(from);++o) if (NpcCombo::OutOf(from,o)==index&&!SameFrame(static_cast<int>(i),index)) { NpcCombo::SetOut(from,o,-1); cut=true; }
    }
    return cut;
}
// After a node is dropped: in the innermost frame under its header (or out of every frame), wires over the frame's
// edge cut, the frames around it grown to hold it.
void Rejoin(HDC dc,int index) {
    if (!At(index)) return;
    Node& node=config_.chain[static_cast<size_t>(index)];
    if (!NpcCombo::Groupable(node)) return;
    const SIZE s=NodeSize(dc,static_cast<size_t>(index));
    const int frame=FrameAt(dc,node.x+s.cx/2.f,node.y+kHeader/2.f,index);
    if (frame!=node.group) {
        node.group=frame;
        if (CutCrossing(index)) DebugHub::Notify(Tr(L"节点移进 / 移出组合：已断开跨过组合边框的连线。").c_str());
    }
    FitFrames(dc,index);
}
// Paint order: frames first (outer under inner), then every other node in chain order; hit tests go the other way.
std::vector<size_t> PaintOrder() {
    std::vector<size_t> frames,others;
    for (size_t i=0;i<config_.chain.size();++i) (IsFrame(static_cast<int>(i))?frames:others).push_back(i);
    std::stable_sort(frames.begin(),frames.end(),[](size_t a,size_t b){ return NpcCombo::GroupDepth(config_,static_cast<int>(a))<NpcCombo::GroupDepth(config_,static_cast<int>(b)); });
    frames.insert(frames.end(),others.begin(),others.end());
    return frames;
}
// A node's place in its frame's order (1-based; 0: in no frame).
int StepNumber(size_t index) {
    const int frame=FrameOf(static_cast<int>(index)); if (frame<0) return 0;
    const std::vector<int> order=NpcCombo::Members(config_,frame);
    const auto at=std::find(order.begin(),order.end(),static_cast<int>(index));
    return at==order.end()?0:static_cast<int>(at-order.begin())+1;
}
// ---- 瞬移 ----
std::wstring DistText(int cm) { wchar_t text[48]{}; _snwprintf_s(text,_countof(text),_TRUNCATE,Tr(L"距目标 %d cm").c_str(),cm); return text; }
std::wstring LiftText(int cm) { wchar_t text[48]{}; _snwprintf_s(text,_countof(text),_TRUNCATE,Tr(L"离地 %d cm").c_str(),cm); return text; }
constexpr int kDistPresets[]={100,150,200,300,500},kLiftPresets[]={0,30,60,100,150};

// ---- node text and size --------------------------------------------------------------------------
std::wstring KeyText() { return Tr(L"NPC 技能键：")+UiHotkeys::Hint(UiHotkeys::Action::NpcSkill); }
// The NPC key's field: "NPC 技能键：" and its binding (for the device in use) as icons.
int KeyUnits(HDC dc) {
    const UiHotkeys::Chord chord=UiHotkeys::Get(UiHotkeys::Action::NpcSkill);
    if (chord.empty()) return Units(dc,UiTheme::Font::Body,KeyText());
    return Units(dc,UiTheme::Font::Body,Tr(L"NPC 技能键："))+static_cast<int>(std::ceil(UiHotkeys::Width(dc,Px(kKeyIcon),chord)/Scale()));
}
void KeyField(HDC dc,const RECT& rect,COLORREF text) {
    const UiHotkeys::Chord chord=UiHotkeys::Get(UiHotkeys::Action::NpcSkill);
    if (chord.empty()) { Label(dc,KeyText(),rect,Scaled(UiTheme::Font::Body),text,DT_SINGLELINE|DT_VCENTER|DT_LEFT|DT_END_ELLIPSIS); return; }
    const std::wstring name=Tr(L"NPC 技能键：");
    Label(dc,name,rect,Scaled(UiTheme::Font::Body),text,DT_SINGLELINE|DT_VCENTER|DT_LEFT);
    const int x=rect.left+static_cast<int>(std::lround(Units(dc,UiTheme::Font::Body,name)*Scale())),h=(std::min)(Px(kKeyIcon),static_cast<int>(rect.bottom-rect.top));
    UiHotkeys::Draw(dc,x,(rect.top+rect.bottom-h)/2,h,chord,rect.right);
}
std::wstring NodeTitle(const Node& node) {
    switch (node.type) {
    case NodeType::Interval: return Tr(L"间隔");
    case NodeType::AutoDodge: return Tr(L"按摇杆方向闪避");
    case NodeType::Trigger: return Tr(node.trigger==static_cast<int>(Trigger::NpcKey)?L"按键":L"替换按键");
    case NodeType::Branch: return Tr(L"按键分支");
    case NodeType::Speed: return Tr(L"加速");
    case NodeType::Weapon: return Tr(L"武器属性");
    case NodeType::Armor: return Tr(L"霸体");
    case NodeType::Invincible: return Tr(L"无敌");
    case NodeType::KeepLook: return Tr(L"保留变身");
    case NodeType::ForceHit: return Tr(L"强制受击");
    case NodeType::Interrupt: return Tr(L"允许中断");
    case NodeType::Grab: return Tr(L"投技");
    case NodeType::BossSkill: return Tr(L"BOSS 技能");
    case NodeType::WeaponMove: return Tr(L"武器派生");
    case NodeType::Damage: return Tr(L"伤害倍率");
    case NodeType::Teleport: return Tr(L"瞬移");
    case NodeType::Group: return Tr(L"组合");
    case NodeType::Look: return Tr(L"变身");
    case NodeType::HeroLook: return Tr(L"变回主角");
    case NodeType::PreInput: return Tr(L"预输入");
    case NodeType::WeaponActions: return Tr(L"武器派生修改");
    default: return MoveText(node.ability);
    }
}
std::wstring ButtonLabel(int button) { return Tr(NpcCombo::TriggerLabel(static_cast<Trigger>(button))); }
// A card's title: a 组合 says how many moves it plays; a node in a 组合 starts with its place in the frame's order.
std::wstring CardTitle(size_t index) {
    const Node& node=config_.chain[index];
    std::wstring title=NodeTitle(node);
    if (node.type==NodeType::Group) {
        wchar_t text[64]{}; _snwprintf_s(text,_countof(text),_TRUNCATE,Tr(L"组合 · %zu 招").c_str(),NpcCombo::GroupSteps(config_,static_cast<int>(index)).size()); title=text;
    }
    if (const int step=StepNumber(index)) title=std::to_wstring(step)+L". "+title;
    return title;
}
std::wstring PauseText(int ms) {
    if (ms<=0) return Tr(L"直接按");
    wchar_t text[48]{}; _snwprintf_s(text,_countof(text),_TRUNCATE,Tr(L"停 %.1f s 后").c_str(),ms/1000.0); return text;
}
std::wstring SpeedText(int percent) { wchar_t text[24]{}; _snwprintf_s(text,_countof(text),_TRUNCATE,L"×%.2f",percent/100.0); return text; }
// The node an exec wire comes into `index` from (-1: none).
int Feeder(int index) {
    for (size_t i=0;i<config_.chain.size();++i) if (NpcCombo::Feeds(config_.chain[i],index)) return static_cast<int>(i);
    return -1;
}
// A move's speed in percent (its own 加速 nodes, else its chain's).
int MoveSpeed(size_t index) { return NpcCombo::SpeedFor(config_,NpcCombo::TriggerOf(config_,index),static_cast<int>(index)); }
// What an interval does to the move before it, once that move has played once.
std::wstring IntervalNote(size_t index) {
    if (!Wired(index)) return Tr(L"未接入派生链");
    int at=static_cast<int>(index),before=-1; bool branch=false;
    std::vector<bool> seen(config_.chain.size(),false);
    while (true) {
        seen[static_cast<size_t>(at)]=true;
        const int feeder=Feeder(at);
        if (feeder<0||seen[static_cast<size_t>(feeder)]) break;
        const Node& n=config_.chain[static_cast<size_t>(feeder)];
        if (NpcCombo::StartsChain(n)) return Tr(L"第一次按键后等待");
        if (n.type==NodeType::Branch) branch=true;
        else if (NpcCombo::IsMove(n)) { before=feeder; break; }
        at=feeder;
    }
    const Node* move=At(before);
    const float length=move&&move->type==NodeType::Skill?NpcCombo::Length(move->ability):0.f;
    if (branch||!(length>0)) return Tr(L"上一招开始后，到时再按才接下一招");
    const double lasts=length*100.0/MoveSpeed(static_cast<size_t>(before));
    const int total=NpcCombo::SharedWait(config_,before);
    wchar_t text[64]{};
    if (total>lasts*1020) _snwprintf_s(text,_countof(text),_TRUNCATE,Tr(L"上一招慢放 ×%.2f").c_str(),lasts*1000.0/total);
    else _snwprintf_s(text,_countof(text),_TRUNCATE,L"%s",Tr(L"到时按键提前接下一招").c_str());
    return text;
}
// What the nodes pointing at a modifier get from it, in words.
std::wstring ModifierNote(size_t index) {
    const Node& node=config_.chain[index];
    const NpcCombo::Pointers from=NpcCombo::PointersTo(config_,static_cast<int>(index));
    if (node.type==NodeType::KeepLook) {
        if (!from.nodes) return Tr(L"未连接：从 BOSS 技能或起端节点拉箭头到这里");
        if (from.triggers) return Tr(L"整条链的 BOSS 技能放完不变回");
        wchar_t text[96]{}; _snwprintf_s(text,_countof(text),_TRUNCATE,Tr(L"指向它的 %d 招 BOSS 技能放完不变回").c_str(),from.nodes); return text;
    }
    if (!from.nodes) return Tr(node.type==NodeType::Weapon?L"未连接：从起端节点拉箭头到这里":L"未连接：从起端节点或招式拉箭头到这里");
    if (node.type==NodeType::Weapon) return Tr(L"这条链的 Boss 招式伤害 × 主武器强度");
    if (node.type==NodeType::Damage) {
        if (from.triggers) return Tr(L"整条链的伤害按这个比例（单独连了伤害倍率的招式除外）");
        wchar_t text[96]{}; _snwprintf_s(text,_countof(text),_TRUNCATE,Tr(L"指向它的 %d 招伤害按这个比例").c_str(),from.nodes); return text;
    }
    if (node.type==NodeType::ForceHit) {
        const std::wstring kind=Tr(NpcCombo::HitKindLabel(node.hit));
        wchar_t text[128]{};
        if (from.triggers) _snwprintf_s(text,_countof(text),_TRUNCATE,Tr(L"整条链打中的敌人被强制%s（无视霸体）").c_str(),kind.c_str());
        else _snwprintf_s(text,_countof(text),_TRUNCATE,Tr(L"指向它的 %d 招打中的敌人被强制%s").c_str(),from.nodes,kind.c_str());
        return text;
    }
    if (node.type==NodeType::Invincible) {
        if (from.triggers) return Tr(L"整条链无敌：招式中不会被打到");
        wchar_t text[96]{}; _snwprintf_s(text,_countof(text),_TRUNCATE,Tr(L"指向它的 %d 招无敌").c_str(),from.nodes); return text;
    }
    const bool speed=node.type==NodeType::Speed,cut=node.type==NodeType::Interrupt;
    if (from.triggers) return Tr(speed?L"整条链加速（单独连了加速的招式除外）":cut?L"整条链随时可被下一次按键打断":L"整条链霸体：受击不被打断");
    wchar_t text[96]{}; _snwprintf_s(text,_countof(text),_TRUNCATE,Tr(speed?L"指向它的 %d 招加速":cut?L"指向它的 %d 招随时可打断":L"指向它的 %d 招霸体").c_str(),from.nodes);
    return text;
}
std::wstring WeaponNote() {
    const NpcCombo::WeaponInfo weapon=NpcCombo::LastWeapon();
    if (!weapon.serial) return Tr(L"放一次后显示当前武器的倍率");
    if (!weapon.ok) return Tr(L"读不到武器强度，按 ×1");
    wchar_t text[160]{}; _snwprintf_s(text,_countof(text),_TRUNCATE,L"%s%s ×%.2f",Tr(L"当前：").c_str(),weapon.name.c_str(),weapon.scale); return text;
}
// The grab module's moves and options (read at most every half second).
const MotianGrab::Snapshot& GrabState() {
    static ULONGLONG at=0; static bool read=false; static MotianGrab::Snapshot grab;
    const ULONGLONG now=GetTickCount64();
    if (!read||now>=at+500) { at=now; read=true; grab=MotianGrab::GetSnapshot(); }
    return grab;
}
std::string Narrow(const std::wstring& text) { std::string out; for (const wchar_t c:text) out+=c<128?static_cast<char>(c):'?'; return out; }
std::wstring GrabName(const MotianGrab::Variant& v) { return v.boss_name+L" · "+v.name; }
// A grab of the current look's boss (MotianGrab boss ids and skin ids fold to one key, GrabBoss::Key).
bool BossGrab(const MotianGrab::Variant& v) { return v.supported&&!moves_.profile.empty()&&GrabBoss::Same(v.boss_id,moves_.profile); }
// The move a 投技 node names, when it still exists and is supported.
const MotianGrab::Variant* NodeGrab(const Node& node) {
    if (node.ability.empty()) return nullptr;
    const std::wstring id(node.ability.begin(),node.ability.end());
    for (const MotianGrab::Variant& v:GrabState().variants) if (v.id==id&&v.supported) return &v;
    return nullptr;
}
// A 投技 node's field: the grab it names (its own pick; the 投技 page's pick and 开启 play no part).
std::wstring GrabField(const Node& node) {
    const MotianGrab::Variant* v=NodeGrab(node);
    return v?GrabName(*v):Tr(node.ability.empty()?L"选择投技":L"所选投技已不可用");
}
std::wstring GrabNote(const Node& node) {
    if (node.ability.empty()) return Tr(L"选一个投技；没选时按键跳过这个节点");
    const MotianGrab::Variant* v=NodeGrab(node);
    if (!v) return Tr(L"所选投技已不可用，请重新选择");
    // The hero takes the look of the grab's boss for it (none when no look plays that boss).
    const std::wstring look=Tr(NpcRetarget::SkinForGrab(v->boss_id,v->source)>=0?L"投时变身为该 Boss":L"这个 Boss 没有外观，不变身");
    const MotianGrab::Snapshot& grab=GrabState();
    if (grab.chain_variant==v->index) return Tr(grab.chain_prepared?L"已预载":L"预载中")+L" · "+look;
    return look+L" · "+Tr(L"不受投技页设置影响");
}
std::wstring NodeNote(size_t index) {
    const Node& node=config_.chain[index];
    const bool wired=Wired(index);
    switch (node.type) {
    case NodeType::Teleport: return wired?Tr(L"瞬身到锁定目标面前（未锁定：最近的敌人）"):Tr(L"未连接");
    case NodeType::Group: {
        const size_t steps=NpcCombo::GroupSteps(config_,static_cast<int>(index)).size();
        if (!steps) return Tr(L"把节点拖进框里：按顺序直接连续释放");
        return wired?Tr(L"框里的招按顺序直接连续释放"):Tr(L"未连接");
    }
    case NodeType::Interval: return IntervalNote(index);
    case NodeType::AutoDodge: return moves_.dodges.empty()?Tr(L"这个外观没有闪避"):wired?Tr(L"按摇杆方向选 Boss 闪避"):Tr(L"未连接");
    case NodeType::Trigger: return {};
    case NodeType::Branch: return Tr(L"上一招后按哪个键，接哪一招");
    case NodeType::Speed: case NodeType::Weapon: case NodeType::Armor: case NodeType::Interrupt: case NodeType::Damage: case NodeType::Invincible: case NodeType::KeepLook: case NodeType::ForceHit: return ModifierNote(index);
    case NodeType::WeaponMove: {
        if (!wired) return Tr(L"未连接");
        return Tr(L"拿这把武器放这一招，放完换回");
    }
    case NodeType::Grab: return wired?GrabNote(node):GrabNote(node)+L" · "+Tr(L"未连接");
    case NodeType::BossSkill: return wired?Tr(L"瞬身变身放这一招，放完变回"):Tr(L"未连接");
    case NodeType::Look:
        if (node.boss.empty()) return Tr(L"选择要变成的 Boss");
        return wired?Tr(L"瞬身变成这个 Boss，派生链结束后保持"):Tr(L"未连接");
    case NodeType::HeroLook: return wired?Tr(L"瞬身变回主角，派生链结束后保持"):Tr(L"未连接");
    case NodeType::PreInput: return Tr(node.ms>0?L"招式中提前按的键保留这么久，没接上就清除":L"提前按的键一直保留到接上招式");
    case NodeType::WeaponActions: return Tr(node.weapon.empty()?L"先选择武器":L"拿这把武器时，下面的动作改放连接的招式");
    default: break;
    }
    if (node.ability.empty()||!MoveOfSkin(node.ability)) return Tr(L"不属于当前外观");
    std::wstring note=Tr(NpcCombo::KindLabel(KindOf(node.ability)));
    const int speed=MoveSpeed(index);
    const float lasts=NpcCombo::Length(node.ability);
    if (lasts>0) note+=L" · "+Seconds(lasts*100.0/speed);
    if (speed!=100) note+=L" · "+SpeedText(speed);
    if (!wired) note+=L" · "+Tr(L"未连接");
    return note;
}
// Width and height in canvas units.
SIZE NodeSize(HDC dc,size_t index) {
    const Node& node=config_.chain[index];
    if (node.type==NodeType::Trigger) {
        const bool key=node.trigger==static_cast<int>(Trigger::NpcKey),execution=ExecutionTrigger(node);
        int field=(key?KeyUnits(dc):Units(dc,UiTheme::Font::Body,ButtonLabel(node.trigger)))+100;
        if (execution) field=(std::max)(field,Units(dc,UiTheme::Font::Body,ExecutionWeaponText(node)+L"  ▾")+60);
        if (KeysTrigger(node)) {
            // The keys as icons: the card grows with them (up to six keys of any kind).
            const bool recording=record_.node==static_cast<int>(index)&&record_.route==-1;
            const bool then_recording=record_.node==static_cast<int>(index)&&record_.route==kRecordThen;
            const int icons=KeyIconsUnits(dc,node.keys,RawTrigger(node),recording,UiTheme::Font::Body);
            const int keys=icons>0?icons:Units(dc,UiTheme::Font::Body,KeysText(node)+L"  ▾");
            // The keys field is the row less 26 units each side and 7 px inside: icons need that much more.
            field=(std::max)(field,(std::max)(keys+(icons>0?70:60),Units(dc,UiTheme::Font::Body,KeysModeText(node)+L"  ▾")+60));
            field=(std::max)(field,Units(dc,UiTheme::Font::Body,ThenText(node,then_recording)+L"  ▾")+60);  // 继续派生
            return {std::clamp(field,300,icons>0?900:480),kHeader+kRow*(RawTrigger(node)?7:6)+kPad};
        }
        return {std::clamp(field,240,350),kHeader+kRow*(key?2:execution?4:3)+kPad};
    }
    if (node.type==NodeType::Branch) {
        // A row each; a 自定义组合键 / 自定义按键 row two (its keys under it).
        int lines=0; int wide=380; bool icons=false;
        for (size_t r=0;r<node.routes.size();++r) {
            const NpcCombo::Route& route=node.routes[r];
            lines+=NpcCombo::KeysRoute(route)?2:1;
            if (!NpcCombo::KeysRoute(route)) continue;
            // Its keys get 11/20 of the line under the row (after a 16-unit indent), its mode the rest: the card grows
            // so both fit, the keys as icons.
            const bool recording=record_.node==static_cast<int>(index)&&record_.route==static_cast<int>(r);
            const int keys=KeyIconsUnits(dc,route.keys,RawRoute(route),recording,UiTheme::Font::Small);icons=icons||keys>0;
            const int keys_units=keys>0?keys:Units(dc,UiTheme::Font::Small,KeysText(route.keys,route.chord,RawRoute(route))+L"  ▾");
            const int mode_units=Units(dc,UiTheme::Font::Small,RouteModeText(route)+L"  ▾");
            wide=(std::max)(wide,(std::max)((keys_units+16)*20/11,(mode_units+16)*20/9)+66);
        }
        return {std::clamp(wide,380,icons?960:560),kHeader+kRow*(lines+2)+kPad};
    }
    if (node.type==NodeType::WeaponActions) {
        // The weapon, the reset time, a note, a row per action (its field, x, its own output), then "+ 添加动作".
        int wide=(std::max)(Units(dc,UiTheme::Font::Strong,CardTitle(index))+70,(std::max)(Units(dc,UiTheme::Font::Body,ActionsWeaponText(node)+L"  ▾")+60,
                                                                                   Units(dc,UiTheme::Font::Small,NodeNote(index))+34));
        for (const NpcCombo::Route& route:node.routes) wide=(std::max)(wide,Units(dc,UiTheme::Font::Small,ButtonLabel(route.button)+L" ▾")+100);
        return {std::clamp(wide,340,520),kHeader+kRow*(static_cast<int>(node.routes.size())+4)+kPad};
    }
    if (node.type==NodeType::Group) return {NpcCombo::ClampGroupW(node.w),NpcCombo::ClampGroupH(node.h)};
    const int title=Units(dc,UiTheme::Font::Strong,CardTitle(index))+70,note=Units(dc,UiTheme::Font::Small,NodeNote(index))+34;
    if (node.type==NodeType::Teleport) {
        const int fields=Units(dc,UiTheme::Font::Body,DistText(NpcCombo::kTeleportDistMax)+L"  ▾")+60;
        return {std::clamp((std::max)((std::max)(title,fields),note),280,420),kHeader+kRow*4+kPad};
    }
    if (node.type==NodeType::Speed||node.type==NodeType::Damage) return {std::clamp((std::max)(title,note),200,340),kHeader+kRow*2+kPad};
    if (node.type==NodeType::WeaponMove) {
        const int weapon=Units(dc,UiTheme::Font::Body,(node.weapon.empty()?Tr(L"选择武器"):WeaponName(node.weapon))+L"  ▾")+60;
        const int move=Units(dc,UiTheme::Font::Body,(node.ability.empty()?Tr(L"选择招式"):WeaponMoveText(node.weapon,node.ability))+L"  ▾")+60;
        return {std::clamp((std::max)((std::max)(title,weapon),(std::max)(move,note)),280,440),kHeader+kRow*4+kPad};
    }
    if (node.type==NodeType::Armor||node.type==NodeType::Interrupt||node.type==NodeType::Invincible||node.type==NodeType::KeepLook) return {std::clamp((std::max)(title,note),200,340),kHeader+kRow+kPad};
    if (node.type==NodeType::BossSkill) {
        const int look=Units(dc,UiTheme::Font::Body,(node.boss.empty()?Tr(L"选择外观"):BossLookName(node.boss))+L"  ▾")+60;
        const int move=Units(dc,UiTheme::Font::Body,(node.ability.empty()?Tr(L"选择招式"):MoveText(node.ability))+L"  ▾")+60;
        return {std::clamp((std::max)((std::max)(title,look),move),280,420),kHeader+kRow*3+kPad};
    }
    if (node.type==NodeType::Look) {
        const int field=Units(dc,UiTheme::Font::Body,(node.boss.empty()?Tr(L"选择外观"):BossLookName(node.boss))+L"  ▾")+60;
        return {std::clamp((std::max)((std::max)(title,note),field),260,420),kHeader+kRow*2+kPad};
    }
    if (node.type==NodeType::HeroLook) return {std::clamp((std::max)(title,note),240,380),kHeader+kRow+kPad};
    if (node.type==NodeType::ForceHit) {
        const int field=Units(dc,UiTheme::Font::Body,Tr(NpcCombo::HitKindLabel(node.hit))+L"  ▾")+60;
        return {std::clamp((std::max)((std::max)(title,note),field),240,400),kHeader+kRow*2+kPad};
    }
    if (node.type==NodeType::Grab) {
        const int field=Units(dc,UiTheme::Font::Body,GrabField(node)+L"  ▾")+40;
        return {std::clamp((std::max)((std::max)(title,note),field),260,420),kHeader+kRow*2+kPad};
    }
    if (node.type==NodeType::Weapon) return {std::clamp((std::max)((std::max)(title,note),Units(dc,UiTheme::Font::Small,WeaponNote())+24),230,360),kHeader+kRow*2+kPad};
    if (node.type==NodeType::Interval) return {std::clamp((std::max)(title,note),190,320),kHeader+kRow*2+kPad};
    if (node.type==NodeType::PreInput) return {std::clamp((std::max)(title,note),230,420),kHeader+kRow*2+kPad};
    return {std::clamp((std::max)(title,note),250,340),kHeader+kRow*2+kPad};  // a move: its note, then its 派生点
}

// ---- canvas <-> screen ---------------------------------------------------------------------------
int ToScreenX(float x) { return static_cast<int>(std::lround(canvas_.left+pan_x_+x*Scale())); }
int ToScreenY(float y) { return static_cast<int>(std::lround(canvas_.top+pan_y_+y*Scale())); }
float ToCanvasX(int x) { return (x-canvas_.left-pan_x_)/Scale(); }
float ToCanvasY(int y) { return (y-canvas_.top-pan_y_)/Scale(); }
int Px(int units) { return (std::max)(1,static_cast<int>(std::lround(units*Scale()))); }
RECT Box(int x,int y,SIZE size) { return {ToScreenX(static_cast<float>(x)),ToScreenY(static_cast<float>(y)),ToScreenX(static_cast<float>(x+size.cx)),ToScreenY(static_cast<float>(y+size.cy))}; }
RECT Row(const RECT& rect,int row,int inset=10) {
    const int top=rect.top+Px(kHeader+row*kRow)+Px(3);
    return {rect.left+Px(inset),top,rect.right-Px(inset),top+Px(kRow-4)};
}
Shape NodeShape(HDC dc,size_t index) {
    const Node& node=config_.chain[index];
    Shape s; s.rect=Box(node.x,node.y,NodeSize(dc,index));
    const int mid=(s.rect.top*2+Px(kHeader))/2;
    s.header={s.rect.left,s.rect.top,s.rect.right,s.rect.top+Px(kHeader)};
    const bool modifier=NpcCombo::IsModifier(node),move=node.type==NodeType::Skill||node.type==NodeType::AutoDodge,setting=NpcCombo::IsPreInput(node);
    s.has_in=!NpcCombo::StartsChain(node)&&!modifier&&!setting; s.has_out=!NpcCombo::HasRows(node)&&!modifier&&!setting;
    s.in={s.rect.left+Px(13),mid}; s.out={s.rect.right-Px(13),mid};
    // Round pins: a trigger's arrow pin sits where an exec input would, a move's just under its exec input; a
    // modifier's (where arrows end) where an exec output would.
    s.has_mod_out=NpcCombo::StartsChain(node)||move||node.type==NodeType::BossSkill||node.type==NodeType::WeaponMove||node.type==NodeType::Teleport||node.type==NodeType::Group||
        node.type==NodeType::Grab;
    if (s.has_mod_out) { const RECT line=Row(s.rect,0); s.mod_out=NpcCombo::StartsChain(node)?s.in:POINT{s.rect.left+Px(13),(line.top+line.bottom)/2}; }
    s.has_mod_in=modifier; s.mod_in=s.out;
    const int r=Px(16);
    s.remove={s.out.x-Px(14)-r,mid-r/2,s.out.x-Px(14),mid+r/2};
    // A value with − / + : an interval's length, a trigger's reset time.
    const auto stepper=[&](int row){
        const RECT line=Row(s.rect,row); const int b=line.bottom-line.top;
        s.minus={line.right-Px(110),line.top,line.right-Px(110)+b,line.bottom}; s.plus={line.right-b,line.top,line.right,line.bottom};
        s.value={s.minus.right+Px(4),line.top,s.plus.left-Px(4),line.bottom};
    };
    if (node.type==NodeType::Interval||setting) {
        const RECT line=Row(s.rect,0); const int b=line.bottom-line.top;
        s.minus={line.left,line.top,line.left+b,line.bottom}; s.plus={line.right-b,line.top,line.right,line.bottom};
        s.value={s.minus.right+Px(4),line.top,s.plus.left-Px(4),line.bottom};
    } else if (node.type==NodeType::Speed||node.type==NodeType::Damage) {
        const RECT line=Row(s.rect,0); const int b=line.bottom-line.top;
        s.minus={line.left,line.top,line.left+b,line.bottom}; s.plus={line.right-b,line.top,line.right,line.bottom};
        s.value={s.minus.right+Px(4),line.top,s.plus.left-Px(4),line.bottom};
    } else if (node.type==NodeType::BossSkill) {
        s.field=Row(s.rect,0,26); s.field2=Row(s.rect,1,26);
        stepper(2);  // its 派生点
    } else if (node.type==NodeType::WeaponMove) {
        s.field=Row(s.rect,0,26); s.field2=Row(s.rect,1,26);  // the weapon, its ability; then a note and the 派生点
        stepper(3);
    } else if (node.type==NodeType::Teleport) {
        s.field=Row(s.rect,0,26); s.field2=Row(s.rect,1,26);  // distance, height; then a note and the 派生点
        stepper(3);
    } else if (node.type==NodeType::Group) {
        stepper(0);  // its 派生点 under the header; the nodes below; the size handle in the corner
        const int h=Px(16);
        s.resize={s.rect.right-h,s.rect.bottom-h,s.rect.right,s.rect.bottom};
    } else if (node.type==NodeType::Grab||node.type==NodeType::Look||node.type==NodeType::ForceHit) {
        s.field=Row(s.rect,0,26);
    } else if (node.type==NodeType::Trigger) {
        s.field=Row(s.rect,0,26);
        stepper(1);
        if (node.trigger!=static_cast<int>(Trigger::NpcKey)) s.toggle=Row(s.rect,2);
        if (ExecutionTrigger(node)) s.field2=Row(s.rect,3,26);  // whose executions: any weapon or one
        if (KeysTrigger(node)) { s.field2=Row(s.rect,3,26); s.field3=Row(s.rect,4,26); }  // its keys; in order or together
        if (RawTrigger(node)) s.toggle2=Row(s.rect,5);  // 吞掉
        if (KeysTrigger(node)) s.field4=Row(s.rect,RawTrigger(node)?6:5,26);  // 继续派生
    } else if (node.type==NodeType::WeaponActions) {
        // The weapon, the reset time, a note, then a row per action (its field, x, its own output) and "+ 添加动作".
        s.field=Row(s.rect,0,26);
        stepper(1);
        int at=3;
        for (size_t k=0;k<node.routes.size();++k) {
            RouteShape route; const RECT line=Row(s.rect,at++); const int b=line.bottom-line.top,right=s.rect.right-Px(30);
            route.remove={right-b,line.top,right,line.bottom};
            route.field={line.left,line.top,route.remove.left-Px(8),line.bottom};
            route.out={s.rect.right-Px(13),(line.top+line.bottom)/2};
            s.routes.push_back(route);
        }
        const RECT add=Row(s.rect,at);
        s.add={add.left,add.top,add.left+Px(132),add.bottom};
    } else if (node.type==NodeType::Branch) {
        // Rows: the note, one per route (button field, pause stepper, x, its own output; a 自定义组合键 / 自定义按键
        // row's keys and mode on a line under it), then "+ 添加按键".
        int at=1;
        for (size_t k=0;k<node.routes.size();++k) {
            RouteShape route; const RECT line=Row(s.rect,at++); const int b=line.bottom-line.top,right=s.rect.right-Px(30);
            route.field={line.left,line.top,line.left+Px(132),line.bottom};
            route.remove={right-b,line.top,right,line.bottom};
            route.plus={route.remove.left-Px(8)-b,line.top,route.remove.left-Px(8),line.bottom};
            route.value={route.plus.left-Px(4)-Px(96),line.top,route.plus.left-Px(4),line.bottom};
            route.minus={route.value.left-Px(4)-b,line.top,route.value.left-Px(4),line.bottom};
            route.out={s.rect.right-Px(13),(line.top+line.bottom)/2};
            if (NpcCombo::KeysRoute(node.routes[k])) {
                const RECT keys=Row(s.rect,at++); const int split=keys.left+Px(16)+(right-keys.left-Px(16))*11/20;
                route.has_keys=true; route.keys={keys.left+Px(16),keys.top,split-Px(3),keys.bottom}; route.mode={split+Px(3),keys.top,right,keys.bottom};
            }
            s.routes.push_back(route);
        }
        const RECT add=Row(s.rect,at);
        s.add={add.left,add.top,add.left+Px(132),add.bottom};
    } else if (move) stepper(1);  // its 派生点
    return s;
}

// ---- editing -------------------------------------------------------------------------------------
// One node selected; a node of the selection keeps the others (so the whole selection can be dragged).
void Select(int node) { selected_=node; if (std::find(picked_.begin(),picked_.end(),node)==picked_.end()) picked_.clear(); }
bool IsPicked(int index) { return index>=0&&(index==selected_||std::find(picked_.begin(),picked_.end(),index)!=picked_.end()); }
// The nodes selected now: the selection, else the one selected node.
std::vector<int> Selection() {
    std::vector<int> out;
    for (const int i:picked_) if (At(i)&&std::find(out.begin(),out.end(),i)==out.end()) out.push_back(i);
    if (out.empty()&&At(selected_)) out.push_back(selected_);
    return out;
}
// The 框选 box on the page, from the press to the pointer.
RECT BoxRect() { return {(std::min)(drag_.start.x,drag_.last.x),(std::min)(drag_.start.y,drag_.last.y),(std::max)(drag_.start.x,drag_.last.x),(std::max)(drag_.start.y,drag_.last.y)}; }
// Ctrl + click: the node joins the selection, or leaves it.
void TogglePick(int node) {
    if (!At(node)) return;
    if (picked_.empty()&&At(selected_)) picked_.push_back(selected_);
    const auto at=std::find(picked_.begin(),picked_.end(),node);
    if (at!=picked_.end()) { picked_.erase(at); if (selected_==node) selected_=picked_.empty()?-1:picked_.back(); }
    else { picked_.push_back(node); selected_=node; }
    if (picked_.size()==1) { selected_=picked_[0]; picked_.clear(); }
}
// A free place for a node of `size` at (x, y) or below it.
POINT FreeSpot(HDC dc,int x,int y,SIZE size,int skip=-1) {
    for (int tries=0;tries<30;++tries) {
        RECT want{x,y,x+size.cx,y+size.cy}; bool clash=false;
        for (size_t i=0;i<config_.chain.size()&&!clash;++i) {
            if (static_cast<int>(i)==skip||(skip>=0&&NpcCombo::InGroup(config_,skip,static_cast<int>(i)))) continue;
            const SIZE s=NodeSize(dc,i); const Node& n=config_.chain[i];
            RECT other{n.x-20,n.y-14,n.x+s.cx+20,n.y+s.cy+14},overlap{};
            if (IntersectRect(&overlap,&want,&other)) clash=true;
        }
        if (!clash) break;
        y+=kHeader+kRow*2+kPad+24;
    }
    return {x,y};
}
bool Room() {
    if (config_.chain.size()<NpcCombo::kMaxNodes) return true;
    DebugHub::Notify(Tr(L"画布最多 64 个节点。").c_str()); return false;
}
void OpenSearch(POINT at,int from=-1,int into=-1,int from_out=0);
void OpenPicker(int node,int route,int part=0);
void Refresh(bool force);
void OpenFiles();
void OpenSaveAs();
// Appends a node after the selected one (or the end of the key's chain), placed to its right.
void Append(Node node) {
    if (!Room()) return;
    const HDC dc=GetDC(window_);
    int after=At(selected_)?selected_:NpcCombo::Tail(config_,NpcCombo::KeyTrigger(config_));
    // After a node in a 组合: into the same frame (or the first frame out that may hold it).
    int frame=FrameOf(after);
    while (frame>=0&&!Fits(node,frame)) { after=frame; frame=FrameOf(frame); }
    node.group=frame;
    const Node* a=At(after);
    node.x=a?a->x+NodeSize(dc,static_cast<size_t>(after)).cx+60:24; node.y=a?a->y:24;
    if (frame>=0&&a) node.y=(std::max)(node.y,config_.chain[static_cast<size_t>(frame)].y+kHeader+kRow+8);
    const int index=NpcCombo::InsertAfter(config_,after,node);
    if (index>=0) {
        const POINT spot=FreeSpot(dc,node.x,node.y,NodeSize(dc,static_cast<size_t>(index)),index);
        config_.chain[static_cast<size_t>(index)].x=spot.x; config_.chain[static_cast<size_t>(index)].y=spot.y;
        FitFrames(dc,index);
        Select(index);
    }
    ReleaseDC(window_,dc);
    Save(); Invalidate();
}
// A node at a canvas point; wired from output `from_out` of `from` / into `into` when given (a wire dropped on
// empty canvas).
int Place(Node node,float x,float y,int from=-1,int into=-1,int from_out=0) {
    if (!Room()) return -1;
    node.x=NpcCombo::ClampPosition(static_cast<int>(x)); node.y=NpcCombo::ClampPosition(static_cast<int>(y)); node.next=-1; node.mods.fill(-1);
    for (NpcCombo::Route& route:node.routes) route.next=-1;
    node.group=-1;
    config_.chain.push_back(node);
    const int index=static_cast<int>(config_.chain.size())-1;
    const HDC dc=GetDC(window_);
    if (NpcCombo::Groupable(node)) {
        const SIZE s=NodeSize(dc,static_cast<size_t>(index));
        config_.chain.back().group=FrameAt(dc,node.x+s.cx/2.f,node.y+kHeader/2.f,index);
    }
    bool refused=false;
    if (from>=0) { if (SameFrame(from,index)) NpcCombo::Link(config_,from,index,static_cast<size_t>((std::max)(0,from_out))); else refused=true; }
    if (into>=0) { if (SameFrame(index,into)) NpcCombo::Link(config_,index,into); else refused=true; }
    FitFrames(dc,index);
    ReleaseDC(window_,dc);
    if (refused) DebugHub::Notify(Tr(L"组合里的节点只和同一个组合里的节点连线；组合本身从框头的引脚接入派生链。").c_str());
    Select(index); Save(); Invalidate();
    return index;
}
void RemoveSelected() {
    std::vector<int> gone=Selection();
    if (gone.size()>1) {
        // The whole selection, last index first (the 按键 and 预输入 nodes stay).
        size_t kept=0;
        gone.erase(std::remove_if(gone.begin(),gone.end(),[&](int i){ const bool keep=IsKey(i)||IsPreInputAt(i); kept+=keep?1:0; return keep; }),gone.end());
        std::sort(gone.rbegin(),gone.rend());
        for (const int i:gone) NpcCombo::Remove(config_,static_cast<size_t>(i));
        wchar_t text[160]{};
        _snwprintf_s(text,_countof(text),_TRUNCATE,Tr(kept?L"已删除 %zu 个节点（「按键」「预输入」节点保留）":L"已删除 %zu 个节点").c_str(),gone.size());
        DebugHub::Notify(text);
        picked_.clear(); Select(-1); Save(); Invalidate(); return;
    }
    if (!At(selected_)) return;
    if (IsKey(selected_)) { DebugHub::Notify(Tr(L"「按键」节点不能删除；断开它的连线即可停用。").c_str()); return; }
    if (IsPreInputAt(selected_)) { DebugHub::Notify(Tr(L"「预输入」节点不能删除；时间设为 0 即一直保留。").c_str()); return; }
    NpcCombo::Remove(config_,static_cast<size_t>(selected_)); picked_.clear(); Select(-1); Save(); Invalidate();
}
// One step up (dir 1) or down (-1) of a node's value: an interval, a trigger's reset time, a 加速's speed, a move's
// 派生点, or the pause of a branch's route `route`.
void Nudge(int index,int dir,int route=-1) {
    if (!At(index)||!dir) return;
    Node& node=config_.chain[static_cast<size_t>(index)];
    dir=dir>0?1:-1;
    if (node.type==NodeType::Interval) node.ms=NpcCombo::ClampInterval(node.ms+dir*NpcCombo::kIntervalStep);
    else if (node.type==NodeType::PreInput) node.ms=NpcCombo::ClampPreInput(node.ms+dir*NpcCombo::kPreInputStep);
    else if (NpcCombo::StartsChain(node)) node.ms=NpcCombo::ClampReset(node.ms+dir*NpcCombo::kResetStep);
    else if (node.type==NodeType::Speed) node.speed=NpcCombo::ClampSpeed(node.speed+dir*NpcCombo::kSpeedStep);
    else if (node.type==NodeType::Damage) node.speed=NpcCombo::ClampDamage(node.speed+dir*NpcCombo::kDamageStep);
    else if (node.type==NodeType::Skill||node.type==NodeType::AutoDodge||node.type==NodeType::BossSkill||node.type==NodeType::WeaponMove||node.type==NodeType::Teleport||node.type==NodeType::Group)
        node.ms=NpcCombo::ClampCancel(node.ms+dir*NpcCombo::kCancelStep);
    else if (node.type==NodeType::Branch&&route>=0&&static_cast<size_t>(route)<node.routes.size())
        node.routes[static_cast<size_t>(route)].pause_ms=NpcCombo::ClampPause(node.routes[static_cast<size_t>(route)].pause_ms+dir*NpcCombo::kPauseStep);
    else return;
    Save(); Invalidate();
}
// A value typed in: a 加速's multiplier (×0.50–×10.00) or a move's 派生点 in seconds (0–10; 0: when it ends). A
// click on the value starts it (the text selected), Enter / a click elsewhere keeps it (clamped), Esc drops it.
bool Typed(const Node& node) {
    return node.type==NodeType::Speed||node.type==NodeType::Damage||node.type==NodeType::Skill||node.type==NodeType::AutoDodge||node.type==NodeType::BossSkill||node.type==NodeType::WeaponMove||
           node.type==NodeType::Teleport||node.type==NodeType::Group||node.type==NodeType::PreInput;
}
void BeginEdit(int index) {
    if (!At(index)||!Typed(config_.chain[static_cast<size_t>(index)])) return;
    const Node& node=config_.chain[static_cast<size_t>(index)];
    wchar_t text[16]{};
    if (node.type==NodeType::Damage) _snwprintf_s(text,_countof(text),_TRUNCATE,L"%d",NpcCombo::ClampDamage(node.speed));
    else if (node.type==NodeType::PreInput) _snwprintf_s(text,_countof(text),_TRUNCATE,L"%.2f",NpcCombo::ClampPreInput(node.ms)/1000.0);
    else _snwprintf_s(text,_countof(text),_TRUNCATE,L"%.2f",node.type==NodeType::Speed?NpcCombo::ClampSpeed(node.speed)/100.0:NpcCombo::ClampCancel(node.ms)/1000.0);
    edit_={index,text,true}; SetFocus(window_); Invalidate();
}
void CancelEdit() { if (edit_.node>=0) { edit_={}; Invalidate(); } }
void CommitEdit() {
    if (edit_.node<0) return;
    const Edit done=edit_; edit_={};
    if (Node* node=At(done.node)?&config_.chain[static_cast<size_t>(done.node)]:nullptr; node&&Typed(*node)&&!done.text.empty()) {
        wchar_t* end=nullptr; const double value=std::wcstod(done.text.c_str(),&end);
        if (end&&end!=done.text.c_str()&&node->type==NodeType::Damage&&value>=0) {
            const int percent=static_cast<int>(std::lround(value));
            node->speed=NpcCombo::ClampDamage(percent);
            if (node->speed!=percent) DebugHub::Notify(Tr(L"伤害倍率在 0% 到 1000% 之间，已调整。").c_str());
            Save();
        } else if (end&&end!=done.text.c_str()&&node->type==NodeType::Speed&&value>0) {
            const int percent=static_cast<int>(std::lround(value*100));
            node->speed=NpcCombo::ClampSpeed(percent);
            if (node->speed!=percent) DebugHub::Notify(Tr(L"加速倍数在 ×0.50 到 ×10.00 之间，已调整。").c_str());
            Save();
        } else if (end&&end!=done.text.c_str()&&node->type==NodeType::PreInput&&value>=0) {
            const int ms=static_cast<int>(std::lround(value*1000));
            node->ms=NpcCombo::ClampPreInput(ms);
            if (node->ms!=ms) DebugHub::Notify(Tr(L"预输入在 0 到 5 秒之间，已调整。").c_str());
            Save();
        } else if (end&&end!=done.text.c_str()&&node->type!=NodeType::Speed&&node->type!=NodeType::Damage&&value>=0) {
            const int ms=static_cast<int>(std::lround(value*1000));
            node->ms=NpcCombo::ClampCancel(ms);
            if (node->ms!=ms) DebugHub::Notify(Tr(L"派生点在 0 到 10 秒之间，已调整。").c_str());
            Save();
        }
    }
    Invalidate();
}
// A typed key while editing; false when it is not for the editor.
bool EditKey(WPARAM key) {
    if (edit_.node<0) return false;
    switch (key) {
    case VK_RETURN: case VK_TAB: CommitEdit(); return true;
    case VK_ESCAPE: CancelEdit(); return true;
    case VK_BACK: if (edit_.fresh) edit_.text.clear(); else if (!edit_.text.empty()) edit_.text.pop_back(); edit_.fresh=false; Invalidate(); return true;
    case VK_DELETE: edit_.text.clear(); edit_.fresh=false; Invalidate(); return true;
    default: return true;  // every other key belongs to the editor while it is open
    }
}
void EditChar(wchar_t c) {
    if (c==L',') c=L'.';
    if (!((c>=L'0'&&c<=L'9')||c==L'.')) return;
    if (edit_.fresh) { edit_.text.clear(); edit_.fresh=false; }
    if (c==L'.'&&edit_.text.find(L'.')!=std::wstring::npos) return;
    if (edit_.text.size()<6) edit_.text.push_back(c);
    Invalidate();
}
// A branch row more (the first hero button no row uses yet, no pause), or one less (one always stays). A 武器派生修改
// row: the first of its usual actions no row has yet.
void AddRoute(int index) {
    if (!At(index)||!NpcCombo::HasRows(config_.chain[static_cast<size_t>(index)])) return;
    Node& node=config_.chain[static_cast<size_t>(index)];
    const bool actions=WeaponActionsNode(node);
    if (node.routes.size()>=NpcCombo::kMaxRoutes) { DebugHub::Notify(Tr(actions?L"武器派生修改最多 6 行。":L"按键分支最多 6 行。").c_str()); return; }
    NpcCombo::Route route;
    if (actions) {
        for (const Trigger t:{Trigger::PerfectDodge,Trigger::Execution,Trigger::SimpleExecution,Trigger::Stance,Trigger::DeflectDash,Trigger::Light,Trigger::Heavy,Trigger::Special,
                              Trigger::CombatArt,Trigger::Shachi,Trigger::Jump,Trigger::AnyDodge})
            if (std::none_of(node.routes.begin(),node.routes.end(),[t](const NpcCombo::Route& r){return r.button==static_cast<int>(t);})) { route.button=static_cast<int>(t); break; }
        node.routes.push_back(route); Save(); Invalidate(); return;
    }
    for (const Trigger t:{Trigger::Light,Trigger::Heavy,Trigger::Shachi,Trigger::Special,Trigger::CombatArt,Trigger::NpcKey,Trigger::Jump,Trigger::AnyDodge})
        if (std::none_of(node.routes.begin(),node.routes.end(),[t](const NpcCombo::Route& r){return r.button==static_cast<int>(t);})) { route.button=static_cast<int>(t); break; }
    node.routes.push_back(route); Save(); Invalidate();
}
void RemoveRoute(int index,int route) {
    if (!At(index)||!NpcCombo::HasRows(config_.chain[static_cast<size_t>(index)])) return;
    Node& node=config_.chain[static_cast<size_t>(index)];
    if (route<0||static_cast<size_t>(route)>=node.routes.size()) return;
    if (node.routes.size()<=1) { DebugHub::Notify(Tr(WeaponActionsNode(node)?L"武器派生修改至少保留一行；不要的话删掉整个节点。":L"按键分支至少保留一行；不要的话删掉整个节点。").c_str()); return; }
    if (record_.node==index&&record_.route>=0) record_={};  // the recorded row may move
    node.routes.erase(node.routes.begin()+route); Save(); Invalidate();
}
// A hero button trigger (or another node chains start from, a 武器派生修改), below everything else; its index (-1:
// no room).
int AddStart(Node node) {
    if (!Room()) return -1;
    const HDC dc=GetDC(window_);
    int bottom=0;
    for (size_t i=0;i<config_.chain.size();++i) bottom=(std::max)(bottom,config_.chain[i].y+static_cast<int>(NodeSize(dc,i).cy));
    node.x=24; node.y=bottom+40;
    config_.chain.push_back(node);
    const int index=static_cast<int>(config_.chain.size())-1;
    const POINT spot=FreeSpot(dc,node.x,node.y,NodeSize(dc,static_cast<size_t>(index)),index);
    config_.chain.back().x=spot.x; config_.chain.back().y=spot.y;
    ReleaseDC(window_,dc);
    Select(index); Save(); Invalidate();
    return index;
}
void AddTrigger() { AddStart(NpcCombo::TriggerNode(Trigger::Light,24,24)); }
// A 加速 / 霸体 / 无敌 / 允许中断 / 武器属性 node above the selected node, which points at it when it may (a trigger; all
// but 武器属性 also a move); else below everything, nothing pointing at it yet.
void AddModifier(NodeType type) {
    if (!Room()) return;
    const HDC dc=GetDC(window_);
    config_.chain.push_back(NpcCombo::ModifierNode(type,24,24));
    const int index=static_cast<int>(config_.chain.size())-1;
    const bool fits=NpcCombo::ModifierFits(config_.chain,selected_,index);
    const SIZE size=NodeSize(dc,static_cast<size_t>(index));
    int x=24,y=24;
    if (fits) { const Node& source=config_.chain[static_cast<size_t>(selected_)]; x=source.x; y=source.y-static_cast<int>(size.cy)-36; NpcCombo::LinkModifier(config_,selected_,index); }
    else { for (size_t i=0;i+1<config_.chain.size();++i) y=(std::max)(y,config_.chain[i].y+static_cast<int>(NodeSize(dc,i).cy)+40); }
    const POINT spot=FreeSpot(dc,x,y,size,index);
    config_.chain.back().x=spot.x; config_.chain.back().y=spot.y;
    ReleaseDC(window_,dc);
    Select(index); Save(); Invalidate();
}
// Canvas pixels from its top to just under the floating tool bar.
float ToolsBottom() {
    LONG bottom=canvas_.top;
    for (const Button b:kTools)
        bottom=(std::max)(bottom,buttons_[static_cast<int>(b)].bottom);
    return static_cast<float>(bottom-canvas_.top+M().S(14));
}
// everything: every node in view (largest zoom up to 1:1 that fits, centred; the top first when even 50 % is too
// tall). Otherwise the opening view: the width fitted at 75 % or more, from the top-left, under the tool bar.
void Frame(bool everything=true) {
    if (!window_||config_.chain.empty()) return;
    const HDC dc=GetDC(window_);
    RECT all{LONG_MAX,LONG_MAX,LONG_MIN,LONG_MIN};
    for (size_t i=0;i<config_.chain.size();++i) {
        const Node& n=config_.chain[i]; const SIZE s=NodeSize(dc,i);
        all.left=(std::min)(all.left,static_cast<LONG>(n.x)); all.top=(std::min)(all.top,static_cast<LONG>(n.y));
        all.right=(std::max)(all.right,static_cast<LONG>(n.x+s.cx)); all.bottom=(std::max)(all.bottom,static_cast<LONG>(n.y+s.cy));
    }
    ReleaseDC(window_,dc);
    const float w=static_cast<float>(canvas_.right-canvas_.left-M().S(48)),h=static_cast<float>(canvas_.bottom-canvas_.top)-ToolsBottom()-M().S(16);
    if (!everything) {
        zoom_index_=2;
        for (int i=4;i>=2;--i) if ((all.right-all.left)*kZooms[i]*Dpi()<=w) { zoom_index_=i; break; }
        pan_x_=M().S(20)-all.left*Scale(); pan_y_=ToolsBottom()-all.top*Scale();
        Invalidate(); return;
    }
    zoom_index_=0;
    for (int i=4;i>=0;--i) if ((all.right-all.left)*kZooms[i]*Dpi()<=w&&(all.bottom-all.top)*kZooms[i]*Dpi()<=h) { zoom_index_=i; break; }
    pan_x_=(canvas_.right-canvas_.left)/2.f-(all.left+all.right)/2.f*Scale();
    pan_y_=ToolsBottom()+h/2.f-(all.top+all.bottom)/2.f*Scale();
    if ((all.bottom-all.top)*Scale()>h) pan_y_=ToolsBottom()-all.top*Scale();
    Invalidate();
}
// Lays out each trigger's chain left to right in rows that fit the canvas at 75 % (the NPC key first): a branch's
// first route goes on along the row, each other route starts a row below what is laid out so far, at the branch's
// right edge; a modifier sits just before the first node pointing at it. Then the nodes nothing reaches. The 预输入
// node goes first, alone at the top-left.
void Arrange() {
    if (!window_) return;
    const HDC dc=GetDC(window_);
    const int width=(std::max)(560,static_cast<int>((canvas_.right-canvas_.left-M().S(64))/(Dpi()*.75f))),gap=60,row_gap=40;
    const int count=static_cast<int>(config_.chain.size());
    std::vector<bool> placed(config_.chain.size(),false);
    // The nodes in a 组合 keep their places in it: only frames outside every frame are laid out, and what they hold
    // moves with them.
    std::vector<POINT> was(config_.chain.size());
    for (size_t i=0;i<config_.chain.size();++i) { was[i]={config_.chain[i].x,config_.chain[i].y}; if (config_.chain[i].group>=0) placed[i]=true; }
    int x=24,y=24,row_h=0,bottom=24;
    const auto place=[&](size_t i,int indent) {
        const SIZE s=NodeSize(dc,i);
        if (x+s.cx>width&&x>indent) { x=indent; y+=row_h+row_gap; row_h=0; }
        config_.chain[i].x=x; config_.chain[i].y=y; x+=s.cx+gap; row_h=(std::max)(row_h,static_cast<int>(s.cy)); placed[i]=true;
        bottom=(std::max)(bottom,y+static_cast<int>(s.cy));
    };
    const auto new_row=[&](int at) { x=at; y=bottom+row_gap; row_h=0; };
    const auto walk=[&](auto&& self,int i,int indent) -> void {
        if (i<0||i>=count||placed[static_cast<size_t>(i)]) return;
        const auto mods=config_.chain[static_cast<size_t>(i)].mods;
        for (const int m:mods) if (m>=0&&m<count&&!placed[static_cast<size_t>(m)]&&NpcCombo::IsModifier(config_.chain[static_cast<size_t>(m)])) place(static_cast<size_t>(m),indent);
        place(static_cast<size_t>(i),indent);
        const Node& node=config_.chain[static_cast<size_t>(i)];
        if (!NpcCombo::HasRows(node)) { self(self,node.next,indent); return; }
        const int right=node.x+static_cast<int>(NodeSize(dc,static_cast<size_t>(i)).cx)+gap;
        const std::vector<NpcCombo::Route> routes=node.routes;
        for (size_t r=0;r<routes.size();++r) {
            const int to=routes[r].next;
            if (to<0||to>=count||placed[static_cast<size_t>(to)]) continue;
            int at=indent;
            if (r>0) { at=right+300<width?right:indent; new_row(at); }
            self(self,to,at);
        }
    };
    // 预输入 (the chain's setting) first, on a row of its own at the top-left.
    if (const int pre=NpcCombo::PreInputNode(config_); pre>=0&&!placed[static_cast<size_t>(pre)]) { place(static_cast<size_t>(pre),24); x=24; y=bottom+row_gap; row_h=0; }
    std::vector<int> triggers;
    const int key=NpcCombo::KeyTrigger(config_); if (key>=0) triggers.push_back(key);
    for (int i=0;i<count;++i) if (NpcCombo::StartsChain(config_.chain[static_cast<size_t>(i)])&&i!=key) triggers.push_back(i);
    bool first=true;
    for (const int t:triggers) {
        if (!first) { x=24; y=bottom+row_gap*2; row_h=0; }
        first=false;
        walk(walk,t,24);
    }
    bool free_row=false;
    for (int i=0;i<count;++i) if (!placed[static_cast<size_t>(i)]) {
        if (!free_row) { x=24; y=bottom+row_gap*2; row_h=0; free_row=true; }
        place(static_cast<size_t>(i),24);
    }
    for (int g=0;g<count;++g) {
        if (!IsFrame(g)||config_.chain[static_cast<size_t>(g)].group>=0) continue;
        const int dx=config_.chain[static_cast<size_t>(g)].x-was[static_cast<size_t>(g)].x,dy=config_.chain[static_cast<size_t>(g)].y-was[static_cast<size_t>(g)].y;
        for (int i=0;i<count;++i) if (NpcCombo::InGroup(config_,i,g)) {
            config_.chain[static_cast<size_t>(i)].x=NpcCombo::ClampPosition(was[static_cast<size_t>(i)].x+dx); config_.chain[static_cast<size_t>(i)].y=NpcCombo::ClampPosition(was[static_cast<size_t>(i)].y+dy);
        }
    }
    ReleaseDC(window_,dc);
    config_.placed=true;
}

// ---- search --------------------------------------------------------------------------------------
std::wstring Lower(std::wstring text) { for (wchar_t& c:text) c=static_cast<wchar_t>(std::towlower(c)); return text; }
void SearchItems() {
    search_.items.clear();
    const auto add=[&](Item item){ item.keys=Lower(item.label+L" "+item.group+L" "+std::wstring(item.ability.begin(),item.ability.end())); search_.items.push_back(std::move(item)); };
    if (search_.mod_from>=0) {
        // From a node's round pin: the modifiers it may point at.
        const std::wstring flow=Tr(L"流程");
        if (At(search_.mod_from)&&config_.chain[static_cast<size_t>(search_.mod_from)].type==NodeType::Grab) {
            add({ItemKind::Interrupt,Tr(L"允许中断"),flow});  // a grab takes 允许中断 only (the boss body plays it out)
            return;
        }
        add({ItemKind::Speed,Tr(L"加速"),flow});
        add({ItemKind::Armor,Tr(L"霸体"),flow});
        add({ItemKind::Invincible,Tr(L"无敌"),flow});
        add({ItemKind::Interrupt,Tr(L"允许中断"),flow});
        add({ItemKind::Damage,Tr(L"伤害倍率"),flow});
        if (At(search_.mod_from)&&NpcCombo::StartsChain(config_.chain[static_cast<size_t>(search_.mod_from)])) add({ItemKind::Weapon,Tr(L"武器属性"),flow});
        if (At(search_.mod_from)) {
            const NodeType from=config_.chain[static_cast<size_t>(search_.mod_from)].type;
            if (NpcCombo::StartsChain(config_.chain[static_cast<size_t>(search_.mod_from)])||from==NodeType::BossSkill||from==NodeType::Group) add({ItemKind::KeepLook,Tr(L"保留变身"),flow});
        }
        add({ItemKind::ForceHit,Tr(L"强制受击"),flow});
        return;
    }
    if (search_.mode==1) {
        // The .combo files in combos\ (the active one first).
        for (const std::wstring& name:files_.names) if (name==files_.active) add({ItemKind::File,name,Tr(L"当前文件")});
        for (const std::wstring& name:files_.names) if (name!=files_.active) add({ItemKind::File,name,Tr(L"combos 文件夹")});
        return;
    }
    if (search_.mode==2) return;  // save-as: SearchFilter builds its one row from the typed name
    if (search_.mode==3) {
        // 派生表: the game's, the hero's, then the looks with a chain in the file, then the others.
        const NpcRetarget::Snapshot snapshot=NpcRetarget::GetSnapshot();
        std::vector<std::string> chained;
        for (const auto& [profile,config]:NpcCombo::All())
            if (std::any_of(config.chain.begin(),config.chain.end(),[](const Node& n){ return NpcCombo::IsMove(n); })) chained.push_back(profile);
        const auto has=[&](const std::string& id){ return std::find(chained.begin(),chained.end(),id)!=chained.end(); };
        const std::wstring top=Tr(L"派生表");
        Item follow{ItemKind::Chain,Tr(L"跟随当前外观"),top}; follow.trigger=kFollowGame; add(follow);
        Item hero{ItemKind::Chain,Tr(L"主角"),top}; hero.trigger=-1; hero.ability=NpcCombo::kHeroProfile; add(hero);
        for (const bool with:{true,false}) for (const NpcRetarget::Entry& e:snapshot.entries) {
            if (has(e.id)!=with) continue;
            Item item{ItemKind::Chain,Tr(e.name.c_str()),Tr(with?L"已有派生链的外观":L"其他外观")}; item.trigger=e.index; item.ability=e.id; add(item);
        }
        return;
    }
    if (search_.pick_node>=0&&At(search_.pick_node)&&config_.chain[static_cast<size_t>(search_.pick_node)].type==NodeType::Look) {
        // A 变身 node's look: every NPC look.
        for (const auto& [id,name]:AllLooks()) { Item item{ItemKind::BossLook,name,Tr(L"变成这个 Boss")}; item.ability=id; add(item); }
        return;
    }
    if (search_.pick_node>=0&&At(search_.pick_node)&&config_.chain[static_cast<size_t>(search_.pick_node)].type==NodeType::BossSkill) {
        const Node& node=config_.chain[static_cast<size_t>(search_.pick_node)];
        if (search_.pick_route==-2) {
            // That look's moves.
            NpcRetarget::SkinMoves look; const int32_t row=BossLookRow(node.boss);
            if (row>=0&&NpcRetarget::GetSkinMoves(row,look))
                for (const std::string& ability:look.skills) { Item item{ItemKind::BossMove,MoveText(ability),Tr(L"招式 · ")+Tr(NpcCombo::KindLabel(NpcCombo::Classify(ability)))}; item.ability=ability; add(item); }
        } else for (const auto& [id,name]:BossLooks()) { Item item{ItemKind::BossLook,name,Tr(L"外观")}; item.ability=id; add(item); }
        return;
    }
    if (search_.pick_node>=0&&At(search_.pick_node)&&WeaponActionsNode(config_.chain[static_cast<size_t>(search_.pick_node)])) {
        // A 武器派生修改's weapon (route -1), or a row's action: the hero's actions (处决 takes the quick ones too unless a
        // 简易处决 row is there).
        if (search_.pick_route<0) {
            for (const auto& [id,shaqi]:ListedWeapons()) { Item item{ItemKind::WeaponPick,WeaponName(id),Tr(shaqi?L"影之武装":L"武器")}; item.ability=id; add(item); }
            return;
        }
        const std::wstring group=Tr(L"改哪个动作");
        for (const Trigger t:NpcCombo::kTriggerOrder) if (NpcCombo::ValidWeaponAction(static_cast<int>(t))) {
            Item item{ItemKind::Button,ButtonLabel(static_cast<int>(t)),group}; item.trigger=static_cast<int>(t); add(item);
        }
        return;
    }
    if (search_.pick_node>=0&&At(search_.pick_node)) {
        // A 武器派生 node's weapon / ability field, or a 处决 trigger's weapon field.
        const Node& node=config_.chain[static_cast<size_t>(search_.pick_node)];
        const bool execution=ExecutionTrigger(node)&&search_.pick_route==-2;
        if (node.type==NodeType::WeaponMove&&search_.pick_route==-2) {
            if (const ComboWeapons::Weapon* w=ComboWeapons::Find(node.weapon))
                for (int kind=0;kind<4;++kind) for (size_t i=0;i<w->count;++i) if (w->moves[i].kind==kind) {
                    Item item{ItemKind::WeaponSkill,WeaponMoveText(node.weapon,w->moves[i].ability),WeaponName(node.weapon)+L" · "+WeaponKindLabel(kind)};
                    item.ability=w->moves[i].ability; add(item);
                }
            return;
        }
        if (node.type==NodeType::WeaponMove||execution) {
            if (execution) add({ItemKind::WeaponPick,Tr(L"任意武器"),Tr(L"处决")});
            for (const auto& [id,shaqi]:ListedWeapons()) if (!execution||HasExecution(id)) {
                Item item{ItemKind::WeaponPick,WeaponName(id),Tr(shaqi?L"影之武装":L"武器")}; item.ability=id; add(item);
            }
            return;
        }
    }
    if (search_.pick_node>=0&&At(search_.pick_node)&&config_.chain[static_cast<size_t>(search_.pick_node)].type==NodeType::ForceHit) {
        // A 强制受击 node's hit reaction.
        for (int k=0;k<static_cast<int>(NpcCombo::HitKind::Count);++k) { Item item{ItemKind::HitKind,Tr(NpcCombo::HitKindLabel(k)),Tr(L"打中的敌人强制")}; item.trigger=k; add(item); }
        return;
    }
    if (search_.pick_node>=0&&search_.pick_route==-4&&At(search_.pick_node)&&KeysTrigger(config_.chain[static_cast<size_t>(search_.pick_node)])) {
        // A 自定义组合键 / 自定义按键's 继续派生: its own keys again, one key of a 自定义按键's kind (recorded, or a pad
        // button / stick way), or one of the hero's buttons / the NPC key.
        const Node& node=config_.chain[static_cast<size_t>(search_.pick_node)];
        const bool raw=RawTrigger(node);
        Item own{ItemKind::ThenKey,Tr(raw?L"再按原按键（默认）":L"再按原组合键（默认）"),Tr(L"继续派生")}; own.trigger=NpcCombo::kThenOwn; add(own);
        if (raw) {
            Item record{ItemKind::ThenKey,Tr(L"录制一个键：按键盘、鼠标侧键、手柄上的键或推左摇杆"),Tr(L"录制")}; record.trigger=-4; add(record);
            const std::wstring pad=Tr(L"手柄按键");
            for (int bit=0;bit<NpcCombo::kPadBits+NpcCombo::kStickWays;++bit) { Item item{ItemKind::ThenKey,PadBothLabel(NpcCombo::kPadKey+bit),pad}; item.trigger=NpcCombo::kThenRaw+NpcCombo::kPadKey+bit; add(item); }
        }
        const std::wstring hero=Tr(L"主角按键");
        const auto button=[&](int b) { if (!NpcCombo::ValidThenButton(b)) return; Item item{ItemKind::ThenKey,ButtonLabel(b),hero}; item.trigger=b; add(item); };
        button(static_cast<int>(Trigger::NpcKey));
        for (const Trigger t:NpcCombo::kTriggerOrder) button(static_cast<int>(t));
        return;
    }
    if (const KeySpec spec=PickedKeys(search_.pick_node,search_.pick_route,search_.pick_part); spec.keys) {
        const std::vector<int>& keys=*spec.keys;
        if (spec.what==1&&spec.raw) {
            // Recording (keyboard / mouse / pad), the pad's buttons by name, then undo / clear / done.
            if (keys.size()<NpcCombo::kComboKeysMax) {
                Item record{ItemKind::RawKey,Tr(L"录制按键：按键盘、鼠标侧键、手柄上的键或推左摇杆"),Tr(L"录制")}; record.trigger=-4; add(record);
                wchar_t group[96]{}; _snwprintf_s(group,_countof(group),_TRUNCATE,Tr(L"手柄按键（%zu / %zu）").c_str(),keys.size(),NpcCombo::kComboKeysMax);
                for (int bit=0;bit<NpcCombo::kPadBits+NpcCombo::kStickWays;++bit) { Item item{ItemKind::RawKey,PadBothLabel(NpcCombo::kPadKey+bit),group}; item.trigger=NpcCombo::kPadKey+bit; add(item); }
            }
            if (!keys.empty()) {
                Item undo{ItemKind::RawKey,Tr(L"删除最后一个按键"),Tr(L"修改")}; undo.trigger=-1; add(undo);
                Item clear{ItemKind::RawKey,Tr(L"清空按键"),Tr(L"修改")}; clear.trigger=-2; add(clear);
            }
            Item done{ItemKind::RawKey,Tr(L"完成"),Tr(L"修改")}; done.trigger=-3; add(done);
            return;
        }
        if (spec.what==1) {
            // Its keys: each pick adds one at the end (the list stays open); then undo / clear.
            wchar_t group[96]{}; _snwprintf_s(group,_countof(group),_TRUNCATE,Tr(L"添加按键（%zu / %zu）").c_str(),keys.size(),NpcCombo::kComboKeysMax);
            if (keys.size()<NpcCombo::kComboKeysMax)
                for (int k=0;k<static_cast<int>(Trigger::Count);++k) if (NpcCombo::ValidComboKey(k)) { Item item{ItemKind::ComboKey,Tr(NpcCombo::ComboKeyLabel(k)),group}; item.trigger=k; add(item); }
            if (!keys.empty()) {
                Item undo{ItemKind::ComboKey,Tr(L"删除最后一个按键"),Tr(L"修改")}; undo.trigger=-1; add(undo);
                Item clear{ItemKind::ComboKey,Tr(L"清空按键"),Tr(L"修改")}; clear.trigger=-2; add(clear);
            }
            Item done{ItemKind::ComboKey,Tr(L"完成"),Tr(L"修改")}; done.trigger=-3; add(done);
            return;
        }
        // How the keys are pressed: together, or in order with the longest gap between two (a few values, or typed).
        add({ItemKind::ComboMode,Tr(L"一起按（150 毫秒内同时按下）"),Tr(L"一起按")});
        search_.items.back().trigger=0;
        const std::wstring order=Tr(L"按顺序按：两键间隔不超过");
        wchar_t typed_text[64]{}; wchar_t* end=nullptr; const long typed=std::wcstol(search_.query.c_str(),&end,10);
        if (!search_.query.empty()&&end&&!*end) {
            _snwprintf_s(typed_text,_countof(typed_text),_TRUNCATE,Tr(L"自定义：%ld 毫秒").c_str(),typed);
            Item item{ItemKind::ComboMode,typed_text,Tr(L"输入的数值")}; item.trigger=static_cast<int>(std::clamp(typed,1L,100000L)); add(item);
            search_.items.back().keys=Lower(search_.query);
        }
        for (const int ms:kKeyWindowPresets) { wchar_t text[32]{}; _snwprintf_s(text,_countof(text),_TRUNCATE,Tr(L"%d 毫秒").c_str(),ms); Item item{ItemKind::ComboMode,text,order}; item.trigger=ms; add(item); }
        if (spec.row&&spec.raw) {
            // A 自定义按键 row's 吞掉 (a trigger has its checkbox).
            const std::wstring actions=Tr(L"这些键原本的游戏动作");
            Item eat{ItemKind::ComboMode,Tr(L"吞掉原动作"),actions}; eat.trigger=-10; add(eat);
            Item keep{ItemKind::ComboMode,Tr(L"不吞掉：照常做游戏动作"),actions}; keep.trigger=-11; add(keep);
        }
        return;
    }
    if (search_.pick_node>=0&&At(search_.pick_node)&&config_.chain[static_cast<size_t>(search_.pick_node)].type==NodeType::Teleport) {
        // A 瞬移 node's distance (route -1) or height (-2): a few values, or the number typed.
        const bool lift=search_.pick_route==-2;
        const std::wstring group=Tr(lift?L"离地高度":L"落点到目标的距离");
        wchar_t typed_text[64]{}; wchar_t* end=nullptr; const long typed=std::wcstol(search_.query.c_str(),&end,10);
        if (!search_.query.empty()&&end&&!*end) {
            _snwprintf_s(typed_text,_countof(typed_text),_TRUNCATE,Tr(L"自定义：%ld cm").c_str(),typed);
            Item item{lift?ItemKind::TeleportLift:ItemKind::TeleportDist,typed_text,Tr(L"输入的数值")}; item.trigger=static_cast<int>(std::clamp(typed,-100000L,100000L)); add(item);
            search_.items.back().keys=Lower(search_.query);
        }
        for (const int cm:lift?std::vector<int>(std::begin(kLiftPresets),std::end(kLiftPresets)):std::vector<int>(std::begin(kDistPresets),std::end(kDistPresets))) {
            wchar_t text[32]{}; _snwprintf_s(text,_countof(text),_TRUNCATE,L"%d cm",cm);
            Item item{lift?ItemKind::TeleportLift:ItemKind::TeleportDist,text,group}; item.trigger=cm; add(item);
        }
        return;
    }
    if (search_.pick_node>=0&&At(search_.pick_node)&&config_.chain[static_cast<size_t>(search_.pick_node)].type==NodeType::Grab) {
        // A 投技 node's field: every supported grab, this look's boss first, then by boss.
        std::vector<const MotianGrab::Variant*> others;
        for (const MotianGrab::Variant& v:GrabState().variants) {
            if (!v.supported) continue;
            if (BossGrab(v)) { Item item{ItemKind::GrabMove,GrabName(v),Tr(L"这个 Boss 的投技")}; item.ability=Narrow(v.id); add(item); }
            else others.push_back(&v);
        }
        std::stable_sort(others.begin(),others.end(),[](const MotianGrab::Variant* a,const MotianGrab::Variant* b){ return a->boss_id<b->boss_id; });
        for (const MotianGrab::Variant* v:others) { Item item{ItemKind::GrabMove,v->name,v->boss_name}; item.ability=Narrow(v->id); add(item); }
        return;
    }
    if (search_.pick_node>=0) {
        // Pick mode: the buttons (a route may be the NPC key too; a trigger's field never is).
        const std::wstring group=Tr(search_.pick_route>=0?L"下一次按的键":L"替换主角按键");
        const auto button=[&](int b) {
            if (search_.pick_route>=0&&!NpcCombo::ValidRouteButton(b)) return;  // an execution is no press of a branch
            Item item{ItemKind::Button,ButtonLabel(b),group}; item.trigger=b; add(item);
        };
        if (search_.pick_route>=0) button(static_cast<int>(Trigger::NpcKey));
        for (const Trigger t:NpcCombo::kTriggerOrder) button(static_cast<int>(t));
        return;
    }
    const std::wstring flow=Tr(L"流程"),buttons=Tr(L"替换主角按键");
    add({ItemKind::Interval,Tr(L"间隔"),flow});
    add({ItemKind::Branch,Tr(L"按键分支"),flow});
    add({ItemKind::Speed,Tr(L"加速"),flow});
    add({ItemKind::Armor,Tr(L"霸体"),flow});
    add({ItemKind::Invincible,Tr(L"无敌"),flow});
    add({ItemKind::KeepLook,Tr(L"保留变身"),flow});
    add({ItemKind::ForceHit,Tr(L"强制受击"),flow});
    add({ItemKind::Interrupt,Tr(L"允许中断"),flow});
    add({ItemKind::Weapon,Tr(L"武器属性"),flow});
    add({ItemKind::Grab,Tr(L"投技"),flow});
    add({ItemKind::BossSkill,Tr(L"BOSS 技能"),flow});
    add({ItemKind::Look,Tr(L"变身"),flow});
    add({ItemKind::HeroLook,Tr(L"变回主角"),flow});
    add({ItemKind::WeaponMove,Tr(L"武器派生"),flow});
    add({ItemKind::WeaponActions,Tr(L"武器派生修改"),flow});
    add({ItemKind::Damage,Tr(L"伤害倍率"),flow});
    add({ItemKind::Group,Tr(L"组合"),flow});
    add({ItemKind::Teleport,Tr(L"瞬移"),flow});
    if (!moves_.dodges.empty()) add({ItemKind::AutoDodge,Tr(L"按摇杆方向闪避"),flow});
    for (const Trigger t:NpcCombo::kTriggerOrder) { Item item{ItemKind::Trigger,Tr(NpcCombo::TriggerLabel(t)),buttons}; item.trigger=static_cast<int>(t); add(item); }
    if (NpcCombo::KeyTrigger(config_)<0) { Item item{ItemKind::Trigger,Tr(L"NPC 技能键"),buttons}; item.trigger=0; add(item); }
    std::vector<std::pair<Kind,std::string>> all;
    for (const std::string& d:moves_.dodges) all.push_back({Kind::Dodge,d});
    for (const std::string& s:moves_.skills) all.push_back({NpcCombo::Classify(s),s});
    for (int k=0;k<static_cast<int>(Kind::Count);++k)
        for (const auto& [kind,ability]:all) if (static_cast<int>(kind)==k) {
            Item item{ItemKind::Move,MoveText(ability),Tr(L"招式 · ")+Tr(NpcCombo::KindLabel(kind))}; item.ability=ability; item.move=kind; add(item);
        }
}
// Rows that match the query: a header per group, then its items.
void SearchFilter() {
    search_.rows.clear();
    if (search_.mode==2) {
        // Save-as: the typed name is the one row.
        search_.items.clear();
        Item item{ItemKind::SaveAs,search_.query.empty()?Tr(L"（输入文件名）"):search_.query+L".combo",{}}; item.keys=search_.query;
        search_.items.push_back(item);
        search_.rows.push_back({true,0,Tr(L"另存为：输入文件名后回车")});
        search_.rows.push_back({false,0,item.label});
        search_.selected=1; search_.scroll=0; return;
    }
    const bool teleport_pick=search_.pick_node>=0&&At(search_.pick_node)&&(config_.chain[static_cast<size_t>(search_.pick_node)].type==NodeType::Teleport||
                             PickedKeys(search_.pick_node,search_.pick_route,search_.pick_part).what==2);
    if (teleport_pick) SearchItems();  // a typed number is a row of its own
    const std::wstring query=Lower(search_.query);
    std::wstring group;
    for (size_t i=0;i<search_.items.size();++i) {
        const Item& item=search_.items[i];
        if (!query.empty()&&item.keys.find(query)==std::wstring::npos) continue;
        if (item.group!=group) { group=item.group; search_.rows.push_back({true,0,group}); }
        search_.rows.push_back({false,i,item.label});
    }
    search_.selected=-1;
    for (size_t r=0;r<search_.rows.size();++r) if (!search_.rows[r].header) { search_.selected=static_cast<int>(r); break; }
    // A picker starts on the button the field has now.
    if (search_.pick_node>=0&&query.empty()&&At(search_.pick_node)) {
        const Node& node=config_.chain[static_cast<size_t>(search_.pick_node)];
        if (node.type==NodeType::BossSkill||node.type==NodeType::Look||node.type==NodeType::WeaponMove||(ExecutionTrigger(node)&&search_.pick_route==-2)||
            (WeaponActionsNode(node)&&search_.pick_route<0)) {
            const std::string& want=node.type==NodeType::BossSkill?(search_.pick_route==-2?node.ability:node.boss):node.type==NodeType::Look?node.boss:
                                    node.type==NodeType::WeaponMove?(search_.pick_route==-2?node.ability:node.weapon):node.weapon;
            for (size_t r=0;r<search_.rows.size();++r) if (!search_.rows[r].header&&search_.items[search_.rows[r].item].ability==want) { search_.selected=static_cast<int>(r); break; }
            search_.scroll=0; return;
        }
        if (const KeySpec spec=PickedKeys(search_.pick_node,search_.pick_route,search_.pick_part); spec.keys) {
            // The keys' list starts on its first key; the mode's on what the node (or row) has now.
            const int want=spec.what==2?(*spec.chord?0:NpcCombo::ClampKeyWindow(*spec.window)):spec.raw?-4:static_cast<int>(Trigger::Light);
            for (size_t r=0;r<search_.rows.size();++r) if (!search_.rows[r].header&&search_.items[search_.rows[r].item].trigger==want) { search_.selected=static_cast<int>(r); break; }
            search_.scroll=0; return;
        }
        if (node.type==NodeType::ForceHit) {
            for (size_t r=0;r<search_.rows.size();++r) if (!search_.rows[r].header&&search_.items[search_.rows[r].item].trigger==NpcCombo::ClampHitKind(node.hit)) { search_.selected=static_cast<int>(r); break; }
            search_.scroll=0; return;
        }
        if (node.type==NodeType::Teleport) {
            const int want=search_.pick_route==-2?node.lift:node.dist;
            for (size_t r=0;r<search_.rows.size();++r) if (!search_.rows[r].header&&search_.items[search_.rows[r].item].trigger==want) { search_.selected=static_cast<int>(r); break; }
            search_.scroll=0; return;
        }
        if (search_.pick_route==-4&&KeysTrigger(node)) {  // 继续派生: on what it has now
            const int want=NpcCombo::HasThen(node)?node.then:NpcCombo::kThenOwn;
            for (size_t r=0;r<search_.rows.size();++r) if (!search_.rows[r].header&&search_.items[search_.rows[r].item].trigger==want) { search_.selected=static_cast<int>(r); break; }
            search_.scroll=0; return;
        }
        if (node.type==NodeType::Grab) {
            if (NodeGrab(node)) for (size_t r=0;r<search_.rows.size();++r) if (!search_.rows[r].header&&search_.items[search_.rows[r].item].ability==node.ability) { search_.selected=static_cast<int>(r); break; }
            search_.scroll=0; return;
        }
        const int now=search_.pick_route>=0&&static_cast<size_t>(search_.pick_route)<node.routes.size()?node.routes[static_cast<size_t>(search_.pick_route)].button:node.trigger;
        for (size_t r=0;r<search_.rows.size();++r) if (!search_.rows[r].header&&search_.items[search_.rows[r].item].trigger==now) { search_.selected=static_cast<int>(r); break; }
    }
    search_.scroll=0;
}
int SearchVisible() { return (std::max)(1,static_cast<int>((search_.list.bottom-search_.list.top)/M().S(26))); }
void SearchLayout() {
    const auto m=M();
    const int w=m.S(320),edit_h=m.S(36),row_h=m.S(26),rows=12,h=edit_h+rows*row_h+m.S(10);
    int x=static_cast<int>(std::lround(canvas_.left+pan_x_+search_.x*Scale())),y=static_cast<int>(std::lround(canvas_.top+pan_y_+search_.y*Scale()));
    x=std::clamp(x,static_cast<int>(canvas_.left)+m.S(6),(std::max)(static_cast<int>(canvas_.left)+m.S(6),static_cast<int>(canvas_.right)-w-m.S(6)));
    y=std::clamp(y,static_cast<int>(canvas_.top)+m.S(6),(std::max)(static_cast<int>(canvas_.top)+m.S(6),static_cast<int>(canvas_.bottom)-h-m.S(6)));
    search_.rect={x,y,x+w,y+h};
    search_.edit={x+m.S(8),y+m.S(8),x+w-m.S(8),y+edit_h};
    search_.list={x+m.S(4),y+edit_h+m.S(4),x+w-m.S(4),y+h-m.S(6)};
}
void SearchEnsureVisible();
void OpenSearch(POINT at,int from,int into,int from_out) {
    if (!HasSkin()) return;
    CommitEdit();
    search_.open=true; search_.query.clear(); search_.from=from; search_.into=into; search_.from_out=from_out; search_.hot=-1;
    search_.pick_node=-1; search_.pick_route=-1; search_.pick_part=0; search_.mod_from=-1; search_.mode=0;
    search_.x=ToCanvasX(at.x); search_.y=ToCanvasY(at.y);
    SearchItems(); SearchFilter(); SearchLayout();
    SetFocus(window_); Invalidate();
}
// The button list under a trigger's field (route -1) or a branch row's field (part 0; 1 its keys, 2 how they are pressed).
void OpenPicker(int node,int route,int part) {
    if (!HasSkin()||!At(node)) return;
    const HDC dc=GetDC(window_); const Shape s=NodeShape(dc,static_cast<size_t>(node)); ReleaseDC(window_,dc);
    RECT field=route==-2?s.field2:route==-3?s.field3:route==-4?s.field4:s.field;
    if (route>=0&&static_cast<size_t>(route)<s.routes.size()) {
        const RouteShape& row=s.routes[static_cast<size_t>(route)];
        field=part==1&&row.has_keys?row.keys:part==2&&row.has_keys?row.mode:row.field;
    }
    OpenSearch({field.left,field.bottom+M().S(2)},-1,-1,0);
    search_.pick_node=node; search_.pick_route=route; search_.pick_part=route>=0?part:0;
    SearchItems(); SearchFilter(); SearchLayout(); SearchEnsureVisible(); Invalidate();
}
// The 文件 button's list (mode 1) and 另存为's name box (mode 2), under the toolbar.
void OpenFileSearch(int mode) {
    const RECT canvas=canvas_;
    OpenSearch({canvas.right-M().S(330),canvas.top+M().S(8)},-1,-1,0);
    if (!search_.open) return;
    search_.mode=mode; files_=NpcCombo::GetFiles();
    SearchItems(); SearchFilter(); SearchLayout(); Invalidate();
}
void OpenFiles() { OpenFileSearch(1); }
// The 派生表 list under the header, on the one edited now.
void OpenChainSearch() {
    CommitEdit();
    search_.open=true; search_.query.clear(); search_.from=-1; search_.into=-1; search_.from_out=0; search_.hot=-1;
    search_.pick_node=-1; search_.pick_route=-1; search_.pick_part=0; search_.mod_from=-1; search_.mode=3;
    search_.x=ToCanvasX(header_.left); search_.y=ToCanvasY(canvas_.top+M().S(8));
    SearchItems(); SearchFilter();
    for (size_t r=0;r<search_.rows.size();++r)
        if (!search_.rows[r].header&&search_.items[search_.rows[r].item].trigger==pinned_) { search_.selected=static_cast<int>(r); break; }
    SearchLayout(); SearchEnsureVisible(); SetFocus(window_); Invalidate();
}
void OpenSaveAs() { OpenFileSearch(2); }
// The modifiers an arrow from node `from`'s round pin, dropped on empty canvas, may point at.
void OpenModSearch(POINT at,int from) {
    OpenSearch(at,-1,-1,0);
    if (!search_.open) return;
    search_.mod_from=from;
    SearchItems(); SearchFilter(); SearchLayout(); Invalidate();
}
void CloseSearch() { if (search_.open) { search_.open=false; Invalidate(); } }
void SearchEnsureVisible() {
    const int visible=SearchVisible();
    if (search_.selected<search_.scroll) search_.scroll=search_.selected;
    if (search_.selected>=search_.scroll+visible) search_.scroll=search_.selected-visible+1;
    search_.scroll=std::clamp(search_.scroll,0,(std::max)(0,static_cast<int>(search_.rows.size())-visible));
}
void SearchMove(int step) {
    if (search_.rows.empty()) return;
    int r=search_.selected<0?0:search_.selected;
    for (int guard=0;guard<static_cast<int>(search_.rows.size());++guard) {
        r=std::clamp(r+step,0,static_cast<int>(search_.rows.size())-1);
        if (!search_.rows[static_cast<size_t>(r)].header) break;
        if (r==0||r==static_cast<int>(search_.rows.size())-1) { step=-step; }
    }
    if (!search_.rows[static_cast<size_t>(r)].header) search_.selected=r;
    SearchEnsureVisible(); Invalidate();
}
// Adds the picked row's node where the search opened (wired when it came from a wire), or sets the picked button.
// 自定义按键 recording: on until Esc, a left click, the page hidden or six keys.
void StopRecording(const wchar_t* note) {
    if (record_.node<0) return;
    record_={};
    if (note) DebugHub::Notify(Tr(note).c_str());
    Invalidate();
}
void StartRecording(int node,int route=-1) {
    if (route==kRecordThen) { if (!At(node)||!RawTrigger(config_.chain[static_cast<size_t>(node)])) return; }
    else { const KeySpec spec=PickedKeys(node,route>=0?route:-2,route>=0?1:0); if (!spec.keys||!spec.raw) return; }
    CloseSearch(); CommitEdit();
    record_={}; record_.node=node; record_.route=route;
    {const UiHotkeys::PadLevel pad=UiHotkeys::CurrentPad(); record_.pad_was=NpcCombo::PadCodeBits(pad.buttons,pad.stick);}
    record_.pad_seeded=true;  // a pad button (or stick way) held now is no press
    SetFocus(window_);
    DebugHub::Notify(Tr(route==kRecordThen?L"录制中：按下继续派生要用的键，Esc 或点击页面取消":L"录制中：按下要加入的键，Esc 或点击页面结束").c_str());
    Layout(); Invalidate();
}
void RecordKey(int code) {
    if (record_.node<0||!At(record_.node)||!NpcCombo::ValidRawKey(code)) return;
    if (record_.route==kRecordThen) {
        Node& node=config_.chain[static_cast<size_t>(record_.node)];
        if (!RawTrigger(node)) { StopRecording(nullptr); return; }
        node.then=NpcCombo::kThenRaw+code; StopRecording(L"已设为继续派生键"); Save(); Layout(); Invalidate(); return;
    }
    const KeySpec spec=PickedKeys(record_.node,record_.route>=0?record_.route:-2,record_.route>=0?1:0);
    if (!spec.keys||!spec.raw) { StopRecording(nullptr); return; }
    std::vector<int>& keys=*spec.keys;
    if (keys.size()<NpcCombo::kComboKeysMax) keys.push_back(code);
    Save(); Layout(); Invalidate();
    if (keys.size()>=NpcCombo::kComboKeysMax) StopRecording(L"已录满 6 个键，录制结束");
}
void SearchPick(int row) {
    if (row<0||static_cast<size_t>(row)>=search_.rows.size()||search_.rows[static_cast<size_t>(row)].header) return;
    const Item item=search_.items[search_.rows[static_cast<size_t>(row)].item];
    const float x=search_.x,y=search_.y; const int from=search_.from,into=search_.into,from_out=search_.from_out,pick=search_.pick_node,route=search_.pick_route,mod_from=search_.mod_from;
    const int part=search_.pick_part;
    CloseSearch();
    if (item.kind==ItemKind::File) {
        if (NpcCombo::SelectFile(item.label)) DebugHub::Notify((Tr(L"已载入派生文件：")+item.label+L".combo").c_str());
        else DebugHub::Notify(StatusText(NpcCombo::GetFiles().status).c_str());
        Refresh(true); return;
    }
    if (item.kind==ItemKind::Chain) {
        pinned_=item.trigger; Refresh(true);
        DebugHub::Notify((pinned_==kFollowGame?Tr(L"派生表跟随当前外观：")+Tr(moves_.name.c_str()):Tr(L"正在编辑派生表：")+Tr(moves_.name.c_str())).c_str());
        return;
    }
    if (item.kind==ItemKind::SaveAs) {
        if (NpcCombo::SaveAs(item.keys)) DebugHub::Notify((Tr(L"已另存为：")+item.keys+L".combo").c_str());
        else DebugHub::Notify(Tr(L"文件名无效或无法写入，未保存").c_str());
        Refresh(true); return;
    }
    if (item.kind==ItemKind::BossLook&&At(pick)&&config_.chain[static_cast<size_t>(pick)].type==NodeType::Look) {
        config_.chain[static_cast<size_t>(pick)].boss=item.ability; Select(pick); Save(); Layout(); Invalidate(); return;
    }
    if (item.kind==ItemKind::BossLook||item.kind==ItemKind::BossMove) {
        if (!At(pick)||config_.chain[static_cast<size_t>(pick)].type!=NodeType::BossSkill) return;
        Node& target=config_.chain[static_cast<size_t>(pick)];
        if (item.kind==ItemKind::BossMove) { target.ability=item.ability; Select(pick); Save(); Invalidate(); return; }
        if (target.boss!=item.ability) { target.boss=item.ability; target.ability.clear(); }
        Select(pick); Save(); Layout();
        if (target.ability.empty()) OpenPicker(pick,-2);  // then its move
        Invalidate(); return;
    }
    if (item.kind==ItemKind::WeaponPick||item.kind==ItemKind::WeaponSkill) {
        if (!At(pick)) return;
        Node& target=config_.chain[static_cast<size_t>(pick)];
        if ((ExecutionTrigger(target)||WeaponActionsNode(target))&&item.kind==ItemKind::WeaponPick) { target.weapon=item.ability; Select(pick); Save(); Layout(); Invalidate(); return; }
        if (target.type!=NodeType::WeaponMove) return;
        if (item.kind==ItemKind::WeaponSkill) { target.ability=item.ability; Select(pick); Save(); Layout(); Invalidate(); return; }
        if (target.weapon!=item.ability) { target.weapon=item.ability; if (!ComboWeapons::MoveOf(target.weapon,target.ability)) target.ability.clear(); }
        Select(pick); Save(); Layout();
        if (target.ability.empty()) OpenPicker(pick,-2);  // then its ability
        Invalidate(); return;
    }
    if (item.kind==ItemKind::TeleportDist||item.kind==ItemKind::TeleportLift) {
        if (!At(pick)||config_.chain[static_cast<size_t>(pick)].type!=NodeType::Teleport) return;
        Node& target=config_.chain[static_cast<size_t>(pick)];
        const bool lift=item.kind==ItemKind::TeleportLift;
        const int value=lift?NpcCombo::ClampTeleportLift(item.trigger):NpcCombo::ClampTeleportDist(item.trigger);
        if (value!=item.trigger) DebugHub::Notify(Tr(lift?L"离地高度在 0 到 300 cm 之间，已调整。":L"落点距离在 50 到 1000 cm 之间，已调整。").c_str());
        (lift?target.lift:target.dist)=value;
        Select(pick); Save(); Layout(); Invalidate(); return;
    }
    if (item.kind==ItemKind::ThenKey) {
        if (!At(pick)||!KeysTrigger(config_.chain[static_cast<size_t>(pick)])) return;
        Node& target=config_.chain[static_cast<size_t>(pick)];
        if (item.trigger==-4) { Select(pick); StartRecording(pick,kRecordThen); return; }
        if (!NpcCombo::ValidThen(target.trigger,item.trigger)) return;
        target.then=item.trigger; Select(pick); Save(); Layout(); Invalidate(); return;
    }
    if (item.kind==ItemKind::ComboKey||item.kind==ItemKind::RawKey) {
        // A trigger's keys (route -2) or a branch row's (part 1).
        const KeySpec spec=PickedKeys(pick,route,part);
        if (!spec.keys||spec.raw!=(item.kind==ItemKind::RawKey)) return;
        std::vector<int>& keys=*spec.keys;
        if (item.trigger==-3) { Select(pick); Invalidate(); return; }  // 完成
        if (item.trigger==-4) { Select(pick); StartRecording(pick,spec.row?route:-1); return; }
        if (item.trigger==-1&&!keys.empty()) keys.pop_back();
        else if (item.trigger==-2) keys.clear();
        else if ((spec.raw?NpcCombo::ValidRawKey(item.trigger):NpcCombo::ValidComboKey(item.trigger))&&keys.size()<NpcCombo::kComboKeysMax) keys.push_back(item.trigger);
        Select(pick); Save(); Layout();
        if (keys.size()<NpcCombo::kComboKeysMax) OpenPicker(pick,route,part);  // the next key
        Invalidate(); return;
    }
    if (item.kind==ItemKind::ComboMode) {
        const KeySpec spec=PickedKeys(pick,route,part);
        if (!spec.keys) return;
        if (item.trigger==-10||item.trigger==-11) *spec.swallow=item.trigger==-10;  // a 自定义按键 row's 吞掉
        else if (item.trigger<=0) *spec.chord=true;
        else {
            *spec.chord=false; *spec.window=NpcCombo::ClampKeyWindow(item.trigger);
            if (*spec.window!=item.trigger) DebugHub::Notify(Tr(L"按顺序的间隔在 100 到 2000 毫秒之间，已调整。").c_str());
        }
        Select(pick); Save(); Layout(); Invalidate(); return;
    }
    if (item.kind==ItemKind::HitKind) {
        if (!At(pick)||config_.chain[static_cast<size_t>(pick)].type!=NodeType::ForceHit) return;
        config_.chain[static_cast<size_t>(pick)].hit=NpcCombo::ClampHitKind(item.trigger);
        Select(pick); Save(); Layout(); Invalidate(); return;
    }
    if (item.kind==ItemKind::GrabMove) {
        if (!At(pick)||config_.chain[static_cast<size_t>(pick)].type!=NodeType::Grab) return;
        config_.chain[static_cast<size_t>(pick)].ability=item.ability;
        Select(pick); Save(); Invalidate(); return;
    }
    if (item.kind==ItemKind::Button) {
        if (!At(pick)) return;
        Node& target=config_.chain[static_cast<size_t>(pick)];
        if (route>=0&&static_cast<size_t>(route)<target.routes.size()) {
            NpcCombo::Route& picked=target.routes[static_cast<size_t>(route)];
            if (picked.button!=item.trigger) { picked.keys.clear(); if (record_.node==pick&&record_.route==route) record_={}; }  // the keys were the other kind's
            picked.button=item.trigger;
        }
        else if (target.type==NodeType::Trigger&&!IsKey(pick)&&item.trigger!=static_cast<int>(Trigger::NpcKey)) {
            target.trigger=item.trigger;
            if (!NpcCombo::ValidThen(target.trigger,target.then)) target.then=NpcCombo::kThenOwn;  // a 自定义按键's key is no 组合键's
            if (record_.node==pick&&record_.route==kRecordThen) record_={};
        }
        Select(pick); Save(); Layout();
        if (KeysTrigger(target)&&target.keys.empty()&&route<0) OpenPicker(pick,-2);  // a new 组合键 / 自定义按键: its keys at once
        if (route>=0&&static_cast<size_t>(route)<target.routes.size()&&NpcCombo::KeysRoute(target.routes[static_cast<size_t>(route)])&&
            target.routes[static_cast<size_t>(route)].keys.empty()) OpenPicker(pick,route,1);  // a row's too
        Invalidate(); return;
    }
    Node node;
    switch (item.kind) {
    case ItemKind::Interval: node.type=NodeType::Interval; node.ms=NpcCombo::kIntervalDefault; break;
    case ItemKind::AutoDodge: node.type=NodeType::AutoDodge; break;
    case ItemKind::Trigger: node=NpcCombo::TriggerNode(static_cast<Trigger>(item.trigger),0,0); break;
    case ItemKind::Branch: node=NpcCombo::BranchNode(0,0); break;
    case ItemKind::Speed: node=NpcCombo::ModifierNode(NodeType::Speed,0,0); break;
    case ItemKind::Weapon: node=NpcCombo::ModifierNode(NodeType::Weapon,0,0); break;
    case ItemKind::Armor: node=NpcCombo::ModifierNode(NodeType::Armor,0,0); break;
    case ItemKind::Invincible: node=NpcCombo::ModifierNode(NodeType::Invincible,0,0); break;
    case ItemKind::KeepLook: node=NpcCombo::ModifierNode(NodeType::KeepLook,0,0); break;
    case ItemKind::ForceHit: node=NpcCombo::ModifierNode(NodeType::ForceHit,0,0); break;
    case ItemKind::Interrupt: node=NpcCombo::ModifierNode(NodeType::Interrupt,0,0); break;
    case ItemKind::Grab: node.type=NodeType::Grab; break;
    case ItemKind::BossSkill: node.type=NodeType::BossSkill; break;
    case ItemKind::Look: node.type=NodeType::Look; break;
    case ItemKind::HeroLook: node.type=NodeType::HeroLook; break;
    case ItemKind::WeaponMove: node.type=NodeType::WeaponMove; break;
    case ItemKind::Damage: node=NpcCombo::ModifierNode(NodeType::Damage,0,0); break;
    case ItemKind::Group: node.type=NodeType::Group; node.w=NpcCombo::kGroupNewW; node.h=NpcCombo::kGroupNewH; break;
    case ItemKind::Teleport: node.type=NodeType::Teleport; break;
    case ItemKind::WeaponActions: node=NpcCombo::WeaponActionsNode(0,0); break;
    default: node.type=NodeType::Skill; node.ability=item.ability; break;
    }
    const bool exec=!NpcCombo::StartsChain(node)&&!NpcCombo::IsModifier(node);
    const int index=Place(node,x,y,exec?from:-1,exec?into:-1,from_out);
    if (index>=0&&mod_from>=0&&NpcCombo::LinkModifier(config_,mod_from,index)) { Save(); Invalidate(); }
    if (index>=0&&(node.type==NodeType::WeaponMove||node.type==NodeType::Grab||node.type==NodeType::Look||WeaponActionsNode(node))) OpenPicker(index,-1);  // its weapon / grab / look at once
    if (index>=0&&KeysTrigger(node)) OpenPicker(index,-2);  // a 组合键: its keys at once
}
int SearchRowAt(POINT p) {
    if (!PtInRect(&search_.list,p)) return -1;
    const int r=search_.scroll+static_cast<int>((p.y-search_.list.top)/M().S(26));
    return r>=0&&static_cast<size_t>(r)<search_.rows.size()?r:-1;
}

// ---- buttons and clicks --------------------------------------------------------------------------
void PressButton(Button button) {
    switch (button) {
    case Button::Toggle: config_.enabled=!config_.enabled; Save(); break;
    case Button::Toast: {
        const bool on=!NpcCombo::ShowToast(); NpcCombo::SetShowToast(on);
        DebugHub::Notify(Tr(on?L"派生提示已打开：派生链放招时右下角显示招式名":L"派生提示已关闭：派生链放招时右下角不再显示招式名").c_str());
        Layout(); Invalidate(); return;
    }
    case Button::Dummy: {  // 测试木桩: the game thread spawns / removes it; the label follows its snapshot (Tick)
        const bool present=TestDummy::GetSnapshot().present; TestDummy::Request(!present);
        if (!present) DebugHub::Notify(Tr(L"测试木桩：在主角面前召唤一个不移动、不攻击、锁血的小怪，用来试派生链").c_str());
        return;
    }
    case Button::Interval: { Node node; node.type=NodeType::Interval; node.ms=NpcCombo::kIntervalDefault; Append(node); return; }
    case Button::Branch: Append(NpcCombo::BranchNode(0,0)); return;
    case Button::Speed: AddModifier(NodeType::Speed); return;
    case Button::Weapon: AddModifier(NodeType::Weapon); return;
    case Button::Armor: AddModifier(NodeType::Armor); return;
    case Button::Invincible: AddModifier(NodeType::Invincible); return;
    case Button::KeepLook: AddModifier(NodeType::KeepLook); return;
    case Button::ForceHit: AddModifier(NodeType::ForceHit); return;
    case Button::Interrupt: AddModifier(NodeType::Interrupt); return;
    case Button::Grab: {
        // A 投技 after the selected move; its grab is picked at once.
        const size_t before=config_.chain.size(); Node node; node.type=NodeType::Grab; Append(node);
        if (config_.chain.size()>before) OpenPicker(static_cast<int>(config_.chain.size())-1,-1);
        return;
    }
    case Button::BossSkill: {
        // A BOSS 技能 after the selected move; its look is picked at once, then its move.
        const size_t before=config_.chain.size(); Node node; node.type=NodeType::BossSkill; Append(node);
        if (config_.chain.size()>before) OpenPicker(static_cast<int>(config_.chain.size())-1,-1);
        return;
    }
    case Button::Look: {
        // A 变身 after the selected move; its look is picked at once.
        const size_t before=config_.chain.size(); Node node; node.type=NodeType::Look; Append(node);
        if (config_.chain.size()>before) OpenPicker(static_cast<int>(config_.chain.size())-1,-1);
        return;
    }
    case Button::HeroLook: { Node node; node.type=NodeType::HeroLook; Append(node); return; }
    case Button::WeaponMove: {
        // A 武器派生 after the selected move; its weapon is picked at once, then its ability.
        const size_t before=config_.chain.size(); Node node; node.type=NodeType::WeaponMove; Append(node);
        if (config_.chain.size()>before) OpenPicker(static_cast<int>(config_.chain.size())-1,-1);
        return;
    }
    case Button::Damage: AddModifier(NodeType::Damage); return;
    case Button::Group: { Node node; node.type=NodeType::Group; node.w=NpcCombo::kGroupNewW; node.h=NpcCombo::kGroupNewH; Append(node); return; }
    case Button::Teleport: { Node node; node.type=NodeType::Teleport; Append(node); return; }
    case Button::AutoDodge: {  // 按摇杆方向闪避: the look's dodge the stick points to when it plays (back when centred)
        if (moves_.dodges.empty()) { DebugHub::Notify(Tr(L"这个外观没有自己的闪避；按摇杆方向闪避只用于有闪避的 Boss 外观").c_str()); return; }
        Node node; node.type=NodeType::AutoDodge; Append(node); return;
    }
    case Button::File: OpenFiles(); return;
    case Button::SaveFile:
        if (HasSkin()) NpcCombo::Save(moves_.profile,config_);
        DebugHub::Notify(NpcCombo::SaveFile()?(Tr(L"已保存：")+files_.active+L".combo").c_str():StatusText(NpcCombo::GetFiles().status).c_str());
        seen_generation_=NpcCombo::Generation(); return;
    case Button::SaveAs: OpenSaveAs(); return;
    case Button::AddTrigger: AddTrigger(); return;
    case Button::WeaponActions: {
        // A 武器派生修改 below everything; its weapon is picked at once.
        const int index=AddStart(NpcCombo::WeaponActionsNode(24,24));
        if (index>=0) OpenPicker(index,-1);
        return;
    }
    case Button::Arrange: Arrange(); Save(); Frame(false); return;
    case Button::Frame: Frame(); return;
    case Button::Remove: RemoveSelected(); return;
    case Button::Clear: {
        // Everything but the NPC key node, which stays unwired where it is.
        const int key=NpcCombo::KeyTrigger(config_);
        Node keep=key>=0?config_.chain[static_cast<size_t>(key)]:NpcCombo::TriggerNode(Trigger::NpcKey,24,24);
        keep.next=-1; keep.mods.fill(-1); config_.chain.assign(1,keep); Select(-1); Save(); break;
    }
    default: break;
    }
    Layout();
}
// A weapon row clicked: a 武器派生 node with that weapon after the selected node, its ability picked at once.
void AddWeaponNode(int row) {
    if (row<0||static_cast<size_t>(row)>=weapon_rows_.size()||weapon_rows_[static_cast<size_t>(row)].header) return;
    const size_t before=config_.chain.size();
    Node node; node.type=NodeType::WeaponMove; node.weapon=weapon_rows_[static_cast<size_t>(row)].id; Append(node);
    if (config_.chain.size()>before) OpenPicker(static_cast<int>(config_.chain.size())-1,-2);
}
int NodeAt(POINT p);
// A weapon row dropped at p: onto a 武器派生 node it becomes that node's weapon; onto a 处决 trigger, the weapon whose
// executions it replaces; on bare canvas, a new 武器派生 node there (unwired, as a dragged move).
void DropWeapon(int row,POINT p) {
    if (row<0||static_cast<size_t>(row)>=weapon_rows_.size()||weapon_rows_[static_cast<size_t>(row)].header) return;
    const std::string id=weapon_rows_[static_cast<size_t>(row)].id;
    const int target=NodeAt(p);
    if (At(target)) {
        Node& t=config_.chain[static_cast<size_t>(target)];
        if (t.type==NodeType::WeaponMove) {
            if (t.weapon!=id) { t.weapon=id; if (!ComboWeapons::MoveOf(t.weapon,t.ability)) t.ability.clear(); }
            Select(target); Save(); Layout(); OpenPicker(target,-2); return;
        }
        if (ExecutionTrigger(t)||WeaponActionsNode(t)) { t.weapon=id; Select(target); Save(); Layout(); Invalidate(); return; }
    }
    Node node; node.type=NodeType::WeaponMove; node.weapon=id;
    const int index=Place(node,ToCanvasX(p.x)-30,ToCanvasY(p.y)-kHeader/2.f);
    if (index>=0) OpenPicker(index,-2);
}
// A click (press and release on the same part without dragging).
void Click(const Hit& hit) {
    switch (hit.part) {
    case Part::Button: PressButton(static_cast<Button>(hit.index)); return;
    case Part::Header: if (search_.open&&search_.mode==3) CloseSearch(); else OpenChainSearch(); return;
    case Part::Pill: if (hit.index>=0&&static_cast<size_t>(hit.index)<pills_.size()) { Node node; node.ability=pills_[static_cast<size_t>(hit.index)].ability; Append(node); } return;
    case Part::Tab: if (hit.index>=0&&static_cast<size_t>(hit.index)<tabs_.size()) { category_=tabs_[static_cast<size_t>(hit.index)].kind; strip_scroll_=0; Layout(); } return;
    case Part::StripLeft: ScrollStrip(-(strip_area_.right-strip_area_.left)*2/3); return;
    case Part::StripRight: ScrollStrip((strip_area_.right-strip_area_.left)*2/3); return;
    case Part::NodeValue: Select(hit.index); if (At(hit.index)&&Typed(config_.chain[static_cast<size_t>(hit.index)])) BeginEdit(hit.index); break;
    case Part::Node: case Part::NodeIn: case Part::NodeOut: case Part::ModOut: case Part::ModIn: case Part::RouteOut: case Part::RouteValue: Select(hit.index); break;
    case Part::NodeMinus: Select(hit.index); Nudge(hit.index,-1); break;
    case Part::NodePlus: Select(hit.index); Nudge(hit.index,1); break;
    case Part::RouteMinus: Select(hit.index); Nudge(hit.index,-1,hit.sub); break;
    case Part::RoutePlus: Select(hit.index); Nudge(hit.index,1,hit.sub); break;
    case Part::RouteRemove: Select(hit.index); RemoveRoute(hit.index,hit.sub); break;
    case Part::RouteAdd: Select(hit.index); AddRoute(hit.index); break;
    case Part::RouteField: Select(hit.index); OpenPicker(hit.index,hit.sub); return;
    case Part::RouteKeys: Select(hit.index); OpenPicker(hit.index,hit.sub,1); return;
    case Part::RouteMode: Select(hit.index); OpenPicker(hit.index,hit.sub,2); return;
    case Part::NodeField: Select(hit.index); if (!IsKey(hit.index)) OpenPicker(hit.index,-1); return;
    case Part::NodeField2: {
        Select(hit.index);
        const Node* n=At(hit.index);
        if (n&&((n->type==NodeType::BossSkill&&!n->boss.empty())||(n->type==NodeType::WeaponMove&&!n->weapon.empty())||ExecutionTrigger(*n)||KeysTrigger(*n)||
                 n->type==NodeType::Teleport)) OpenPicker(hit.index,-2);
        return;
    }
    case Part::NodeField3: Select(hit.index); if (At(hit.index)&&KeysTrigger(config_.chain[static_cast<size_t>(hit.index)])) OpenPicker(hit.index,-3); return;
    case Part::NodeField4: Select(hit.index); if (At(hit.index)&&KeysTrigger(config_.chain[static_cast<size_t>(hit.index)])) OpenPicker(hit.index,-4); return;
    case Part::WeaponRow: AddWeaponNode(hit.index); return;
    case Part::NodeToggle: if (At(hit.index)) { Select(hit.index); config_.chain[static_cast<size_t>(hit.index)].on=!config_.chain[static_cast<size_t>(hit.index)].on; Save(); } break;
    case Part::NodeToggle2:
        if (At(hit.index)&&RawTrigger(config_.chain[static_cast<size_t>(hit.index)])) {
            Select(hit.index); Node& n=config_.chain[static_cast<size_t>(hit.index)]; n.swallow=!n.swallow; Save();
        }
        break;
    case Part::NodeRemove: picked_.clear(); Select(hit.index); RemoveSelected(); return;  // that node alone
    case Part::Canvas: Select(-1); break;
    default: break;
    }
    Invalidate();
}

// ---- layout --------------------------------------------------------------------------------------
std::wstring ButtonText(Button button) {
    switch (button) {
    case Button::Toggle: return Tr(config_.enabled?L"派生链：开":L"派生链：关");
    case Button::Toast: return Tr(NpcCombo::ShowToast()?L"派生提示：开":L"派生提示：关");
    case Button::Dummy: return Tr(dummy_present_?L"移除测试木桩":L"召唤测试木桩");
    case Button::Interval: return Tr(L"+ 间隔");
    case Button::Branch: return Tr(L"+ 按键分支");
    case Button::Speed: return Tr(L"+ 加速");
    case Button::Weapon: return Tr(L"+ 武器属性");
    case Button::Armor: return Tr(L"+ 霸体");
    case Button::Invincible: return Tr(L"+ 无敌");
    case Button::KeepLook: return Tr(L"+ 保留变身");
    case Button::ForceHit: return Tr(L"+ 强制受击");
    case Button::Interrupt: return Tr(L"+ 允许中断");
    case Button::Grab: return Tr(L"+ 投技");
    case Button::BossSkill: return Tr(L"+ BOSS 技能");
    case Button::Look: return Tr(L"+ 变身");
    case Button::HeroLook: return Tr(L"+ 变回主角");
    case Button::WeaponMove: return Tr(L"+ 武器派生");
    case Button::WeaponActions: return Tr(L"+ 武器派生修改");
    case Button::Damage: return Tr(L"+ 伤害倍率");
    case Button::Group: return Tr(L"+ 组合");
    case Button::Teleport: return Tr(L"+ 瞬移");
    case Button::AutoDodge: return Tr(L"+ 按摇杆方向闪避");
    case Button::File: return Tr(L"文件：")+(files_.active.empty()?std::wstring(L"-"):files_.active)+L" ▾";
    case Button::SaveFile: return Tr(L"保存");
    case Button::SaveAs: return Tr(L"另存为");
    case Button::AddTrigger: return Tr(L"+ 替换按键");
    case Button::Arrange: return Tr(L"整理");
    case Button::Frame: return Tr(L"显示全部");
    case Button::Remove: return Tr(L"删除");
    case Button::Clear: return Tr(L"清空");
    default: return {};
    }
}
std::wstring TabText(const Tab& tab) {
    wchar_t text[64]{};
    _snwprintf_s(text,_countof(text),_TRUNCATE,L"%s %zu",tab.kind<0?Tr(L"全部").c_str():Tr(NpcCombo::KindLabel(static_cast<Kind>(tab.kind))).c_str(),tab.count);
    return text;
}
// Top to bottom: the skin and the chain switch, the kind tabs, one horizontal strip of the tab's moves, then the
// canvas taking every pixel that is left (the hub gives the page at least the visible height), then one status line.
void Layout() {
    if (!window_) return;
    RECT client{}; GetClientRect(window_,&client);
    const auto m=M(); const int pad=m.Margin(),gap=m.S(8),width=(std::max)(m.S(420),static_cast<int>(client.right));
    const int right=width-pad;
    const HDC dc=GetDC(window_);
    const HFONT body=Font(UiTheme::Font::Body),strong=Font(UiTheme::Font::Strong),small_font=Font(UiTheme::Font::Small);
    int y=m.S(12);
    const int row=m.S(32);
    const auto button_w=[&](Button b){ return static_cast<int>(Extent(dc,b==Button::Toggle?strong:small_font,ButtonText(b)).cx)+m.S(b==Button::Toggle?28:20); };
    const int toggle_w=button_w(Button::Toggle);
    buttons_[static_cast<int>(Button::Toggle)]={right-toggle_w,y,right,y+row};
    const int toast_w=button_w(Button::Toast);  // 派生提示: the notice in the game's bottom-right corner
    buttons_[static_cast<int>(Button::Toast)]={right-toggle_w-gap-toast_w,y,right-toggle_w-gap,y+row};
    const int dummy_w=button_w(Button::Dummy);  // 测试木桩: a mob that stands still and does not die, to try chains on
    const int dummy_right=right-toggle_w-toast_w-2*gap;
    buttons_[static_cast<int>(Button::Dummy)]={dummy_right-dummy_w,y,dummy_right,y+row};
    header_={pad,y,dummy_right-dummy_w-gap,y+row};
    header_.right=(std::min)(header_.right,pad+static_cast<LONG>(Extent(dc,HasSkin()?strong:body,HeaderText()).cx)+m.S(20));
    y+=row+m.S(10);
    // Kind tabs (全部 first).
    std::vector<std::pair<Kind,std::string>> all;
    if (HasSkin()) {
        for (const std::string& d:moves_.dodges) all.push_back({Kind::Dodge,d});
        for (const std::string& s:moves_.skills) all.push_back({NpcCombo::Classify(s),s});
    }
    tabs_.clear();
    const int tab_h=m.S(28);
    tabs_title_={pad,y,pad+static_cast<int>(Extent(dc,strong,Tr(L"Boss 招式")).cx)+m.S(14),y+tab_h};
    int tx=tabs_title_.right;
    std::vector<Tab> wanted{{-1,{},all.size()}};
    for (int k=0;k<static_cast<int>(Kind::Count);++k) {
        const size_t count=static_cast<size_t>(std::count_if(all.begin(),all.end(),[k](const auto& a){return static_cast<int>(a.first)==k;}));
        if (count) wanted.push_back({k,{},count});
    }
    if (category_>=0&&std::none_of(wanted.begin(),wanted.end(),[](const Tab& t){return t.kind==category_;})) { category_=-1; strip_scroll_=0; }
    if (HasSkin()) for (Tab tab:wanted) {
        const int w=static_cast<int>(Extent(dc,small_font,TabText(tab)).cx)+m.S(22);
        if (tx+w>right&&tx>tabs_title_.right) { tx=tabs_title_.right; y+=tab_h+m.S(6); }
        tab.rect={tx,y,tx+w,y+tab_h}; tabs_.push_back(tab); tx+=w+m.S(6);
    }
    y+=tab_h+m.S(8);
    // The tab's moves in one row, scrolled sideways (wheel, or the arrows when they do not fit).
    const int strip_h=m.S(34);
    strip_={pad,y,right,y+strip_h};
    pills_.clear();
    int total=0;
    std::vector<std::pair<int,int>> spans;
    for (const auto& [kind,ability]:all) {
        if (category_>=0&&static_cast<int>(kind)!=category_) continue;
        const int w=static_cast<int>(Extent(dc,body,MoveText(ability)).cx)+m.S(24);
        spans.push_back({total,w}); pills_.push_back({{},ability,kind}); total+=w+m.S(6);
    }
    total=(std::max)(0,total-m.S(6));
    const int arrow=m.S(30);
    strip_area_=strip_; strip_left_={}; strip_right_={};
    if (total>strip_.right-strip_.left) {
        strip_left_={strip_.left,strip_.top,strip_.left+arrow,strip_.bottom};
        strip_right_={strip_.right-arrow,strip_.top,strip_.right,strip_.bottom};
        strip_area_={strip_left_.right+m.S(6),strip_.top,strip_right_.left-m.S(6),strip_.bottom};
    }
    strip_scroll_=std::clamp(strip_scroll_,0,(std::max)(0,total-static_cast<int>(strip_area_.right-strip_area_.left)));
    for (size_t i=0;i<pills_.size();++i) {
        const int x=strip_area_.left+spans[i].first-strip_scroll_;
        pills_[i].rect={x,strip_.top+m.S(2),x+spans[i].second,strip_.bottom-m.S(2)};
    }
    y=strip_.bottom+m.S(10);
    // The canvas: everything above the status line.
    const int footer_h=m.S(24),min_canvas=m.S(320);
    canvas_={pad,y,right,(std::max)(y+min_canvas,static_cast<int>(client.bottom)-m.S(10)-footer_h-m.S(6))};
    footer_={pad,canvas_.bottom+m.S(6),right,canvas_.bottom+m.S(6)+footer_h};
    // The weapon list: a column on the canvas's right, when the canvas keeps room beside it (a narrow page adds a
    // weapon with + 武器派生 and the node's weapon field instead).
    weapon_rows_.clear(); weapons_={}; weapons_title_={};
    const int panel=m.S(176),row_h=m.S(30);
    if (HasSkin()&&right-pad-panel-gap>=m.S(440)) {
        weapons_={right-panel,canvas_.top,right,canvas_.bottom}; canvas_.right=weapons_.left-gap;
        weapons_title_={weapons_.left+m.S(10),weapons_.top+m.S(6),weapons_.right-m.S(8),weapons_.top+m.S(30)};
        std::vector<WeaponRow> rows; bool shaqi_header=false;
        for (const auto& [id,shaqi]:ListedWeapons()) {
            if (shaqi&&!shaqi_header) { shaqi_header=true; WeaponRow h; h.header=true; h.name=Tr(L"影之武装"); rows.push_back(h); }
            WeaponRow r; r.id=id; r.shaqi=shaqi; r.name=WeaponName(id);
            const std::wstring wide(id.begin(),id.end()); r.image=WeaponIcons::Find(wide.c_str(),shaqi).image; rows.push_back(r);
        }
        const int top=weapons_title_.bottom+m.S(4),visible=static_cast<int>(weapons_.bottom)-m.S(6)-top;
        weapon_scroll_=std::clamp(weapon_scroll_,0,(std::max)(0,static_cast<int>(rows.size())*row_h-visible));
        for (size_t i=0;i<rows.size();++i) {
            const int ry=top+static_cast<int>(i)*row_h-weapon_scroll_;
            rows[i].rect={weapons_.left+m.S(6),ry,weapons_.right-m.S(6),ry+row_h-m.S(2)};
        }
        weapon_rows_=std::move(rows);
    }
    // Canvas tools: a floating bar in the canvas's top-left corner (wrapping when the canvas is narrow).
    {
        // The first row leaves the zoom label room; the rows under it take the canvas's width.
        const int tool_h=m.S(28),inset=m.S(10),first=canvas_.right-m.S(110),rest=canvas_.right-inset;
        int x=canvas_.left+inset,top=canvas_.top+inset;
        for (auto b=std::rbegin(kTools);b!=std::rend(kTools);++b) {
            const int w=button_w(*b),limit=top==canvas_.top+inset?first:rest;
            if (x+w>limit&&x>canvas_.left+inset) { x=canvas_.left+inset; top+=tool_h+m.S(6); }
            buttons_[static_cast<int>(*b)]={x,top,x+w,top+tool_h}; x+=w+m.S(6);
        }
    }
    const int minimum=y+min_canvas+m.S(6)+footer_h+m.S(10);
    const std::wstring key=UiHotkeys::Hint(UiHotkeys::Action::NpcSkill);
    wchar_t hint[1024]{};
    _snwprintf_s(hint,_countof(hint),_TRUNCATE,Tr(L"每按一次 %s（或被替换的主角按键）放下一招；招式中按下的键会记住，到它的派生点（未设则放完）时接上 · 按键分支：按不同的键、或在可接下一招后停顿再按，接不同的招 · 从起端节点或招式左侧的圆点拉箭头到加速 / 霸体 / 允许中断 / 武器属性：起端节点＝整条链，招式＝只这一招，武器属性只接起端节点 · 点加速的倍数或招式的派生点可直接输入 · Tab / 右键搜索添加 · 从引脚拉线连接，拉到空白处搜索并接上，Alt + 点引脚断开 · 空白处拖动平移，按住 Ctrl / Shift 拖动框选（选中的节点一起拖动、一起删除），Ctrl + 点节点加选 / 取消 · 滚轮缩放 · Home 显示全部").c_str(),key.c_str());
    hint_text_=hint;
    ReleaseDC(window_,dc);
    if (search_.open) SearchLayout();
    if (minimum!=height_) { height_=minimum; DebugHub::SetMinimum(window_,m.S(460),height_); }
    Invalidate();
}
void ScrollStrip(int delta) { strip_scroll_+=delta; Layout(); }

// ---- hit testing ---------------------------------------------------------------------------------
bool Near(POINT a,POINT b,int r) { return (a.x-b.x)*(a.x-b.x)+(a.y-b.y)*(a.y-b.y)<=r*r; }
Hit HitTest(POINT p) {
    const auto in=[&](const RECT& r){return PtInRect(&r,p)!=FALSE;};
    if (search_.open&&in(search_.rect)) return {Part::Search,SearchRowAt(p)};
    if (HasSkin()&&in(buttons_[static_cast<int>(Button::Toggle)])) return {Part::Button,static_cast<int>(Button::Toggle)};
    if (in(buttons_[static_cast<int>(Button::Toast)])) return {Part::Button,static_cast<int>(Button::Toast)};
    if (in(buttons_[static_cast<int>(Button::Dummy)])) return {Part::Button,static_cast<int>(Button::Dummy)};
    if (in(header_)) return {Part::Header,-1};
    if (HasSkin()) for (const Button b:kTools) if (in(buttons_[static_cast<int>(b)])) return {Part::Button,static_cast<int>(b)};
    for (size_t i=0;i<tabs_.size();++i) if (in(tabs_[i].rect)) return {Part::Tab,static_cast<int>(i)};
    if (in(strip_left_)) return {Part::StripLeft,-1};
    if (in(strip_right_)) return {Part::StripRight,-1};
    if (in(strip_area_)) for (size_t i=0;i<pills_.size();++i) if (in(pills_[i].rect)) return {Part::Pill,static_cast<int>(i)};
    if (HasSkin()&&in(weapons_)) {
        const int top=weapons_title_.bottom+M().S(4);
        if (p.y>=top) for (size_t i=0;i<weapon_rows_.size();++i) if (!weapon_rows_[i].header&&in(weapon_rows_[i].rect)) return {Part::WeaponRow,static_cast<int>(i)};
        return {};
    }
    if (!in(canvas_)||!HasSkin()) return {};
    const HDC dc=GetDC(window_);
    const int pin=(std::max)(M().S(9),Px(kPin+4));
    Hit hit{Part::Canvas,-1};
    const std::vector<size_t> order=PaintOrder();
    for (size_t k=order.size();k-->0&&hit.part==Part::Canvas;) {
        const size_t n=order[k];
        const Node& node=config_.chain[n]; const Shape s=NodeShape(dc,n); RECT grown=s.rect; InflateRect(&grown,pin,0);
        if (!in(grown)) continue;
        const int i=static_cast<int>(n);
        if (node.type==NodeType::Group) {
            // A frame answers on its header row, its 派生点 row and its corner; its body is canvas (pan, search).
            if (s.has_out&&Near(p,s.out,pin)) hit={Part::NodeOut,i};
            else if (s.has_in&&Near(p,s.in,pin)) hit={Part::NodeIn,i};
            else if (s.has_mod_out&&Near(p,s.mod_out,pin)) hit={Part::ModOut,i};
            else if (!in(s.rect)) continue;
            else if (in(s.resize)) hit={Part::GroupResize,i};
            else if ((IsPicked(i)||hot_.index==i)&&in(s.remove)) hit={Part::NodeRemove,i};
            else if (in(s.minus)) hit={Part::NodeMinus,i};
            else if (in(s.plus)) hit={Part::NodePlus,i};
            else if (in(s.value)) hit={Part::NodeValue,i};
            else if (p.y<Row(s.rect,0).bottom+Px(3)) hit={Part::Node,i};
            continue;
        }
        int route_out=-1;
        for (size_t r=0;r<s.routes.size();++r) if (Near(p,s.routes[r].out,pin)) { route_out=static_cast<int>(r); break; }
        const bool stepper=node.type==NodeType::Interval||node.type==NodeType::Trigger||node.type==NodeType::Speed||node.type==NodeType::Damage||node.type==NodeType::Skill||
                           node.type==NodeType::AutoDodge||node.type==NodeType::BossSkill||node.type==NodeType::WeaponMove||node.type==NodeType::Teleport||node.type==NodeType::PreInput||
                           node.type==NodeType::WeaponActions;
        if (route_out>=0) hit={Part::RouteOut,i,route_out};
        else if (s.has_out&&Near(p,s.out,pin)) hit={Part::NodeOut,i};
        else if (s.has_in&&Near(p,s.in,pin)) hit={Part::NodeIn,i};
        else if (s.has_mod_out&&Near(p,s.mod_out,pin)) hit={Part::ModOut,i};
        else if (s.has_mod_in&&Near(p,s.mod_in,pin)) hit={Part::ModIn,i};
        else if (!in(s.rect)) continue;
        else if ((IsPicked(i)||hot_.index==i)&&!IsKey(i)&&!IsPreInputAt(i)&&in(s.remove)) hit={Part::NodeRemove,i};
        else if (stepper&&in(s.minus)) hit={Part::NodeMinus,i};
        else if (stepper&&in(s.plus)) hit={Part::NodePlus,i};
        else if (stepper&&in(s.value)) hit={Part::NodeValue,i};
        else if (node.type==NodeType::Trigger&&!IsKey(i)&&in(s.field)) hit={Part::NodeField,i};
        else if (node.type==NodeType::Grab&&in(s.field)) hit={Part::NodeField,i};
        else if (node.type==NodeType::WeaponActions&&in(s.field)) hit={Part::NodeField,i};
        else if (node.type==NodeType::Look&&in(s.field)) hit={Part::NodeField,i};
        else if (node.type==NodeType::ForceHit&&in(s.field)) hit={Part::NodeField,i};
        else if (node.type==NodeType::BossSkill&&in(s.field)) hit={Part::NodeField,i};
        else if (node.type==NodeType::BossSkill&&in(s.field2)) hit={Part::NodeField2,i};
        else if (node.type==NodeType::WeaponMove&&in(s.field)) hit={Part::NodeField,i};
        else if (node.type==NodeType::WeaponMove&&in(s.field2)) hit={Part::NodeField2,i};
        else if (node.type==NodeType::Teleport&&in(s.field)) hit={Part::NodeField,i};
        else if (node.type==NodeType::Teleport&&in(s.field2)) hit={Part::NodeField2,i};
        else if (ExecutionTrigger(node)&&in(s.field2)) hit={Part::NodeField2,i};
        else if (KeysTrigger(node)&&in(s.field2)) hit={Part::NodeField2,i};
        else if (KeysTrigger(node)&&in(s.field3)) hit={Part::NodeField3,i};
        else if (KeysTrigger(node)&&in(s.field4)) hit={Part::NodeField4,i};
        else if (node.type==NodeType::Trigger&&!IsKey(i)&&in(s.toggle)) hit={Part::NodeToggle,i};
        else if (RawTrigger(node)&&in(s.toggle2)) hit={Part::NodeToggle2,i};
        else if (NpcCombo::HasRows(node)) {
            hit={Part::Node,i};
            for (size_t r=0;r<s.routes.size();++r) {
                const RouteShape& route=s.routes[r]; const int row=static_cast<int>(r);
                if (in(route.field)) hit={Part::RouteField,i,row};
                else if (in(route.minus)) hit={Part::RouteMinus,i,row};
                else if (in(route.plus)) hit={Part::RoutePlus,i,row};
                else if (in(route.value)) hit={Part::RouteValue,i,row};
                else if (in(route.remove)) hit={Part::RouteRemove,i,row};
                else if (route.has_keys&&in(route.keys)) hit={Part::RouteKeys,i,row};
                else if (route.has_keys&&in(route.mode)) hit={Part::RouteMode,i,row};
                else continue;
                break;
            }
            if (hit.part==Part::Node&&in(s.add)) hit={Part::RouteAdd,i};
        }
        else hit={Part::Node,i};
    }
    ReleaseDC(window_,dc);
    return hit;
}
// The node under a point, for dropping a wire (-1: none).
int NodeAt(POINT p) {
    const Hit hit=HitTest(p);
    return hit.part>=Part::Node&&hit.part<=Part::ModIn?hit.index:-1;
}

// ---- painting ------------------------------------------------------------------------------------
// printed: WM_PRINTCLIENT's DC (hub transitions, the offline preview); otherwise a WM_PAINT.
template<class Draw> void Buffered(HWND window,HDC printed,Draw draw) {
    PAINTSTRUCT paint{}; const HDC target=printed?printed:BeginPaint(window,&paint);
    RECT client{}; GetClientRect(window,&client);
    const int width=(std::max)(1L,client.right),height=(std::max)(1L,client.bottom);
    const HDC dc=CreateCompatibleDC(target);
    const HBITMAP bitmap=dc?CreateCompatibleBitmap(target,width,height):nullptr;
    const HGDIOBJ old=bitmap?SelectObject(dc,bitmap):nullptr;
    const HDC surface=bitmap?dc:target;
    FillRect(surface,&client,UiTheme::Background()); SetBkMode(surface,TRANSPARENT);
    { Gdiplus::Graphics graphics(surface); graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias); draw(surface,graphics,client); }
    if (bitmap) { BitBlt(target,0,0,width,height,dc,0,0,SRCCOPY); SelectObject(dc,old); DeleteObject(bitmap); }
    if (dc) DeleteDC(dc);
    if (!printed) EndPaint(window,&paint);
}
void DrawButton(HDC dc,Gdiplus::Graphics& graphics,Button button,bool on=false,bool enabled=true) {
    const RECT& rect=buttons_[static_cast<int>(button)];
    const auto m=M(); const bool hot=enabled&&hot_==Hit{Part::Button,static_cast<int>(button)},down=hot&&pressed_==hot_;
    const COLORREF paper=Paper(),accent=UiTheme::Accent();
    COLORREF fill=on?UiTheme::Mix(paper,accent,40):UiTheme::Mix(paper,UiTheme::Base(),45),edge=on?accent:UiTheme::Mix(UiTheme::Border(),paper,35);
    if (hot) fill=UiTheme::Mix(fill,UiTheme::Text(),down?16:8);
    Rounded(graphics,rect,static_cast<float>(m.S(8)),fill,255,edge,255);
    Label(dc,ButtonText(button),rect,Font(button==Button::Toggle?UiTheme::Font::Strong:UiTheme::Font::Small),enabled?UiTheme::Text():UiTheme::MutedText(),DT_SINGLELINE|DT_VCENTER|DT_CENTER|DT_END_ELLIPSIS);
}
// Exec pin: the Blueprint pentagon, filled when wired.
void ExecPin(Gdiplus::Graphics& graphics,POINT at,bool wired,bool hot,COLORREF color) {
    const float r=static_cast<float>(Px(kPin))*(hot?1.25f:1.f),x=static_cast<float>(at.x),y=static_cast<float>(at.y);
    Gdiplus::PointF points[5]={{x-r,y-r},{x+r*.15f,y-r},{x+r,y},{x+r*.15f,y+r},{x-r,y+r}};
    if (wired) { Gdiplus::SolidBrush brush(Argb(color)); graphics.FillPolygon(&brush,points,5); }
    Gdiplus::Pen pen(Argb(color),(std::max)(1.2f,1.5f*Zoom())); graphics.DrawPolygon(&pen,points,5);
}
// A modifier's pin: a circle, filled when wired.
void ModPin(Gdiplus::Graphics& graphics,POINT at,bool wired,bool hot,COLORREF color) {
    const float r=static_cast<float>(Px(kPin))*(hot?1.2f:.95f);
    const Gdiplus::RectF box(static_cast<float>(at.x)-r,static_cast<float>(at.y)-r,2*r,2*r);
    if (wired) { Gdiplus::SolidBrush brush(Argb(color)); graphics.FillEllipse(&brush,box); }
    Gdiplus::Pen pen(Argb(color),(std::max)(1.2f,1.5f*Zoom())); graphics.DrawEllipse(&pen,box);
}
// A modifier's colour (its card, pin and the arrows into it); high contrast: the system highlight for all of them.
COLORREF ModColor(NodeType type) {
    if (UiTheme::HighContrast()) return UiTheme::AccentBlue();
    if (type==NodeType::Weapon) return UiTheme::Mix(RGB(214,160,60),UiTheme::Text(),15);
    if (type==NodeType::Armor) return UiTheme::Mix(RGB(200,96,70),UiTheme::Text(),15);
    if (type==NodeType::Invincible) return UiTheme::Mix(RGB(232,196,84),UiTheme::Text(),12);
    if (type==NodeType::KeepLook) return UiTheme::Mix(RGB(168,120,232),UiTheme::Text(),15);
    if (type==NodeType::ForceHit) return UiTheme::Mix(RGB(236,128,48),UiTheme::Text(),15);
    if (type==NodeType::Interrupt) return UiTheme::Mix(RGB(70,190,150),UiTheme::Text(),15);
    if (type==NodeType::Damage) return UiTheme::Mix(RGB(226,72,112),UiTheme::Text(),15);
    return UiTheme::AccentBlue();
}
COLORREF GrabColor() { return UiTheme::HighContrast()?UiTheme::AccentBlue():UiTheme::Mix(RGB(190,104,170),UiTheme::Text(),15); }
COLORREF BranchColor() { return UiTheme::Mix(RGB(150,120,210),UiTheme::Text(),15); }
COLORREF GroupColor() { return UiTheme::HighContrast()?UiTheme::AccentBlue():UiTheme::Mix(RGB(120,170,120),UiTheme::Text(),18); }
COLORREF TeleportColor() { return UiTheme::HighContrast()?UiTheme::AccentBlue():UiTheme::Mix(RGB(110,140,230),UiTheme::Text(),15); }
// Where an arrow from `from` meets the card `rect`: on its way to the card's middle, just outside the card's edge (so
// the head is never under the card it points at); the card's pin `inside` when `from` is on the card itself.
POINT ArrowTip(POINT from,const RECT& rect,POINT inside) {
    const float gap=static_cast<float>(Px(3));
    const float l=rect.left-gap,t=rect.top-gap,r=rect.right+gap,b=rect.bottom+gap;
    const float fx=static_cast<float>(from.x),fy=static_cast<float>(from.y);
    if (fx>=l&&fx<=r&&fy>=t&&fy<=b) return inside;
    const float dx=(rect.left+rect.right)*.5f-fx,dy=(rect.top+rect.bottom)*.5f-fy;
    float s=0;
    if (dx>0) s=(std::max)(s,(l-fx)/dx); else if (dx<0) s=(std::max)(s,(r-fx)/dx);
    if (dy>0) s=(std::max)(s,(t-fy)/dy); else if (dy<0) s=(std::max)(s,(b-fy)/dy);
    return {static_cast<LONG>(std::lround(fx+dx*s)),static_cast<LONG>(std::lround(fy+dy*s))};
}
float ArrowHeadLength() { return (std::max)(12.f,20.f*Zoom()); }
// A point well inside the head of an arrow from `from` with its tip at `tip` (the offline preview reads its colour).
POINT ArrowHeadPoint(POINT from,POINT tip) {
    const float dx=static_cast<float>(tip.x-from.x),dy=static_cast<float>(tip.y-from.y),length=std::sqrt(dx*dx+dy*dy);
    if (length<1) return tip;
    const float back=ArrowHeadLength()*.45f;
    return {static_cast<LONG>(std::lround(tip.x-dx/length*back)),static_cast<LONG>(std::lround(tip.y-dy/length*back))};
}
// A straight arrow from a node's round pin to its tip: a solid shaft from the pin's edge, then a filled, notched
// head (20 px long at 100 %, never under 12 px) edged in the canvas colour so it reads on the grid, on a wire and on
// a card alike. The canvas draws the shaft under the cards (Shaft) and every head again over them (Head).
enum class ArrowPart { Shaft, Head, Both };
void Arrow(Gdiplus::Graphics& graphics,POINT from,POINT tip,COLORREF color,BYTE alpha,float width,ArrowPart part=ArrowPart::Both) {
    const float dx=static_cast<float>(tip.x-from.x),dy=static_cast<float>(tip.y-from.y),length=std::sqrt(dx*dx+dy*dy);
    const float r=static_cast<float>(Px(kPin))+1.f,head=ArrowHeadLength();
    if (length<=r+head*.5f) return;
    const float ux=dx/length,uy=dy/length,bx=tip.x-ux*head,by=tip.y-uy*head,half=head*.5f;
    const float nx=bx+ux*head*.25f,ny=by+uy*head*.25f;  // the notch at the back of the head
    Gdiplus::Pen shaft(Argb(color,alpha),width); shaft.SetStartCap(Gdiplus::LineCapRound);
    if (part==ArrowPart::Shaft) { graphics.DrawLine(&shaft,from.x+ux*r,from.y+uy*r,nx+ux*2.f,ny+uy*2.f); return; }
    const Gdiplus::PointF points[]={{static_cast<float>(tip.x),static_cast<float>(tip.y)},{bx-uy*half,by+ux*half},{nx,ny},{bx+uy*half,by-ux*half}};
    Gdiplus::GraphicsPath path; path.AddPolygon(points,4);
    Gdiplus::Pen edge(Argb(UiTheme::Base(),alpha),(std::max)(2.f,3.f*Zoom())); edge.SetLineJoin(Gdiplus::LineJoinRound);
    graphics.DrawPath(&edge,&path);
    // The shaft again over the edge's notch (all of it, or the stretch just behind the head), then the head.
    const float back=part==ArrowPart::Both?length-r:(std::min)(length-r,head*1.6f);
    graphics.DrawLine(&shaft,tip.x-ux*back,tip.y-uy*back,nx+ux*2.f,ny+uy*2.f);
    Gdiplus::SolidBrush fill(Argb(color,alpha)); graphics.FillPath(&fill,&path);
}
void Wire(Gdiplus::Graphics& graphics,POINT from,POINT to,COLORREF color,BYTE alpha,float width) {
    const float dx=(std::max)(static_cast<float>(Px(50)),std::fabs(static_cast<float>(to.x-from.x))*.5f);
    Gdiplus::Pen pen(Argb(color,alpha),width);
    graphics.DrawBezier(&pen,static_cast<float>(from.x+Px(kPin)),static_cast<float>(from.y),from.x+Px(kPin)+dx,static_cast<float>(from.y),
                        to.x-Px(kPin)-dx,static_cast<float>(to.y),static_cast<float>(to.x-Px(kPin)),static_cast<float>(to.y));
}
// The node card shared by every node kind: soft shadow, body, tinted header, selection / playing outline.
void Card(HDC dc,Gdiplus::Graphics& graphics,const Shape& s,COLORREF hue,const std::wstring& title,bool selected,bool playing,bool dim,bool show_remove,bool remove_hot) {
    const COLORREF panel=UiTheme::Panel(),text=UiTheme::Text(),accent=UiTheme::Accent();
    const float radius=static_cast<float>(Px(8));
    RECT shadow=s.rect; OffsetRect(&shadow,Px(2),Px(3));
    Rounded(graphics,shadow,radius,RGB(0,0,0),45,0,0);
    Rounded(graphics,s.rect,radius,UiTheme::Mix(panel,UiTheme::Base(),25),dim?200:245,0,0);
    Rounded(graphics,s.header,radius,UiTheme::Mix(panel,hue,dim?28:52),255,0,0,1.f,true);
    const COLORREF edge=playing?UiTheme::AccentBlue():selected?accent:UiTheme::Mix(UiTheme::Border(),panel,25);
    Rounded(graphics,s.rect,radius,0,0,edge,255,selected||playing?(std::max)(1.8f,2.4f*Zoom()):1.f);
    const bool left_pin=s.has_in||(s.has_mod_out&&s.mod_out.y==s.in.y),right_pin=s.has_out||s.has_mod_in;
    RECT title_rect{(left_pin?s.in.x:s.rect.left)+Px(left_pin?12:10),s.header.top,(show_remove?s.remove.left:right_pin?s.out.x-Px(10):s.rect.right-Px(10))-Px(4),s.header.bottom};
    Label(dc,title,title_rect,Scaled(UiTheme::Font::Strong),dim?UiTheme::MutedText():text,DT_SINGLELINE|DT_VCENTER|DT_LEFT|DT_END_ELLIPSIS);
    if (show_remove) {
        Rounded(graphics,s.remove,static_cast<float>(Px(8)),remove_hot?UiTheme::Danger():UiTheme::Mix(panel,UiTheme::Danger(),35),255,0,0);
        Label(dc,L"×",s.remove,Scaled(UiTheme::Font::Small),remove_hot?RGB(255,255,255):text,DT_SINGLELINE|DT_VCENTER|DT_CENTER);
    }
}
// − value + ; minus / plus / value: their hit parts.
void StepperAt(HDC dc,Gdiplus::Graphics& graphics,const RECT& minus,const RECT& value_rect,const RECT& plus,const std::wstring& value,const Hit& minus_hit,const Hit& plus_hit,const Hit& value_hit,
               UiTheme::Font font=UiTheme::Font::Strong) {
    const COLORREF accent=UiTheme::Accent(),text=UiTheme::Text();
    for (const auto& [rect,sign,hit]:{std::tuple{minus,L"−",minus_hit},std::tuple{plus,L"+",plus_hit}}) {
        const bool on=hot_==hit;
        Rounded(graphics,rect,static_cast<float>(Px(5)),UiTheme::Mix(UiTheme::Base(),accent,on?40:12),255,UiTheme::Mix(UiTheme::Border(),accent,40),255);
        Label(dc,sign,rect,Scaled(UiTheme::Font::Strong),text,DT_SINGLELINE|DT_VCENTER|DT_CENTER);
    }
    const bool value_hot=hot_==value_hit;
    Rounded(graphics,value_rect,static_cast<float>(Px(5)),UiTheme::Mix(UiTheme::Base(),accent,value_hot?16:4),255,UiTheme::Mix(UiTheme::Border(),UiTheme::Base(),20),255);
    Label(dc,value,value_rect,Scaled(font),text,DT_SINGLELINE|DT_VCENTER|DT_CENTER|DT_END_ELLIPSIS);
}
void Stepper(HDC dc,Gdiplus::Graphics& graphics,const Shape& s,int index,const std::wstring& value) {
    StepperAt(dc,graphics,s.minus,s.value,s.plus,value,Hit{Part::NodeMinus,index},Hit{Part::NodePlus,index},Hit{Part::NodeValue,index});
}
void DrawSearch(HDC dc,Gdiplus::Graphics& graphics) {
    const auto m=M(); const COLORREF panel=UiTheme::Panel(),text=UiTheme::Text(),muted=UiTheme::MutedText(),accent=UiTheme::Accent();
    RECT shadow=search_.rect; OffsetRect(&shadow,m.S(3),m.S(4));
    Rounded(graphics,shadow,static_cast<float>(m.S(10)),RGB(0,0,0),70,0,0);
    Rounded(graphics,search_.rect,static_cast<float>(m.S(10)),UiTheme::Mix(panel,UiTheme::Base(),15),255,UiTheme::Mix(UiTheme::Border(),accent,35),255,1.5f);
    Rounded(graphics,search_.edit,static_cast<float>(m.S(7)),UiTheme::Base(),255,accent,255);
    RECT input=search_.edit; InflateRect(&input,-m.S(10),0);
    const bool empty=search_.query.empty();
    const bool caret=(GetTickCount64()/530)%2==0;
    Label(dc,empty?Tr(L"搜索招式、间隔、替换按键…"):search_.query+(caret?L"▏":L" "),input,Font(UiTheme::Font::Body),empty?muted:text,DT_SINGLELINE|DT_VCENTER|DT_LEFT|DT_END_ELLIPSIS);
    const int row_h=m.S(26),visible=SearchVisible();
    const int saved=SaveDC(dc);
    IntersectClipRect(dc,search_.list.left,search_.list.top,search_.list.right,search_.list.bottom);
    for (int r=search_.scroll;r<search_.scroll+visible&&static_cast<size_t>(r)<search_.rows.size();++r) {
        const SearchRow& row=search_.rows[static_cast<size_t>(r)];
        const int top=search_.list.top+(r-search_.scroll)*row_h;
        RECT line{search_.list.left,top,search_.list.right,top+row_h};
        if (row.header) { RECT t=line; t.left+=m.S(8); Label(dc,row.text,t,Font(UiTheme::Font::Small),muted,DT_SINGLELINE|DT_VCENTER|DT_LEFT|DT_END_ELLIPSIS); continue; }
        const Item& item=search_.items[row.item];
        const bool sel=r==search_.selected,hot=r==search_.hot;
        if (sel||hot) Rounded(graphics,line,static_cast<float>(m.S(6)),UiTheme::Mix(panel,accent,sel?32:14),255,0,0);
        const COLORREF hue=item.kind==ItemKind::Move?KindColor(item.move):item.kind==ItemKind::Trigger?TriggerColor(item.trigger):UiTheme::AccentBlue();
        const RECT dot{line.left+m.S(14),top+row_h/2-m.S(4),line.left+m.S(22),top+row_h/2+m.S(4)};
        Rounded(graphics,dot,static_cast<float>(m.S(4)),hue,255,0,0);
        RECT t=line; t.left+=m.S(30); t.right-=m.S(8);
        Label(dc,row.text,t,Font(UiTheme::Font::Body),text,DT_SINGLELINE|DT_VCENTER|DT_LEFT|DT_END_ELLIPSIS);
    }
    RestoreDC(dc,saved);
    if (search_.rows.empty()) Label(dc,Tr(L"没有匹配的项"),search_.list,Font(UiTheme::Font::Small),muted,DT_SINGLELINE|DT_VCENTER|DT_CENTER);
}
void DrawCanvas(HDC dc,Gdiplus::Graphics& graphics) {
    const auto m=M(); const COLORREF base=UiTheme::Base(),border=UiTheme::Border(),text=UiTheme::Text(),muted=UiTheme::MutedText(),accent=UiTheme::Accent();
    Rounded(graphics,canvas_,static_cast<float>(m.S(10)),base,255,0,0);
    const int saved=SaveDC(dc);
    IntersectClipRect(dc,canvas_.left+1,canvas_.top+1,canvas_.right-1,canvas_.bottom-1);
    Gdiplus::GraphicsPath clip; RoundedPath(clip,static_cast<float>(canvas_.left),static_cast<float>(canvas_.top),static_cast<float>(canvas_.right-canvas_.left),static_cast<float>(canvas_.bottom-canvas_.top),static_cast<float>(m.S(10)));
    graphics.SetClip(&clip);
    // Grid: minor lines every 16 units, major every 128, as in the Blueprint editor, in the theme's border colour.
    {
        const float step=kGrid*Scale();
        const float x0=canvas_.left+std::fmod(pan_x_,step)-step,y0=canvas_.top+std::fmod(pan_y_,step)-step;
        Gdiplus::Pen minor(Argb(UiTheme::Mix(base,border,16)),1.f),major(Argb(UiTheme::Mix(base,border,45)),1.f);
        for (float x=x0;x<canvas_.right;x+=step) {
            const long u=std::lround((x-canvas_.left-pan_x_)/Scale());
            if (step>=6||u%kMajor==0) graphics.DrawLine(u%kMajor==0?&major:&minor,x,static_cast<float>(canvas_.top),x,static_cast<float>(canvas_.bottom));
        }
        for (float y=y0;y<canvas_.bottom;y+=step) {
            const long u=std::lround((y-canvas_.top-pan_y_)/Scale());
            if (step>=6||u%kMajor==0) graphics.DrawLine(u%kMajor==0?&major:&minor,static_cast<float>(canvas_.left),y,static_cast<float>(canvas_.right),y);
        }
    }
    // Which nodes play: reached from a trigger that is on; a modifier when a node pointing at it does.
    const size_t count=config_.chain.size();
    std::vector<bool> live(count,false),fed(count,false),modded(count,false),pointed(count,false);
    for (size_t t=0;t<count;++t) {
        const Node& n=config_.chain[t];
        if (!NpcCombo::StartsChain(n)) continue;
        live[t]=WeaponActionsNode(n)?n.on&&!n.weapon.empty():n.on||IsKey(static_cast<int>(t));
        if (live[t]) for (const size_t i:NpcCombo::Order(config_,static_cast<int>(t))) live[i]=true;
    }
    for (size_t i=0;i<count;++i) {
        const Node& n=config_.chain[i];
        for (int slot=0;slot<NpcCombo::kModSlots;++slot) {
            const int mod=NpcCombo::ModOf(config_,static_cast<int>(i),slot); if (mod<0) continue;
            modded[i]=true; pointed[static_cast<size_t>(mod)]=true;
        }
        for (size_t o=0;o<NpcCombo::OutCount(n);++o) { const int to=NpcCombo::OutOf(n,o); if (to>=0&&static_cast<size_t>(to)<count) fed[static_cast<size_t>(to)]=true; }
    }
    // The nodes in a 组合 play when it does.
    for (int pass=0;pass<NpcCombo::kGroupDepthMax;++pass)
        for (size_t i=0;i<count;++i) { const int f=config_.chain[i].group; if (f>=0&&static_cast<size_t>(f)<count&&live[static_cast<size_t>(f)]) live[i]=true; }
    for (size_t i=0;i<count;++i) for (int slot=0;slot<NpcCombo::kModSlots;++slot) {
        const int mod=NpcCombo::ModOf(config_,static_cast<int>(i),slot);
        if (mod>=0&&live[i]) live[static_cast<size_t>(mod)]=true;
    }
    std::vector<Shape> shapes; shapes.reserve(count);
    for (size_t i=0;i<count;++i) shapes.push_back(NodeShape(dc,i));
    const std::vector<size_t> order=PaintOrder();
    // 组合 frames under everything: a tinted body, the header with its pins, its 派生点, the size handle.
    for (const size_t i:order) {
        const Node& node=config_.chain[i]; if (node.type!=NodeType::Group) continue;
        const Shape& s=shapes[i]; const int index=static_cast<int>(i);
        const bool selected=IsPicked(index),hot=hot_.index==index&&hot_.part>=Part::Node&&hot_.part<=Part::NodeRemove;
        const COLORREF hue=GroupColor(),panel=UiTheme::Panel();
        const float radius=static_cast<float>(Px(10));
        Rounded(graphics,s.rect,radius,UiTheme::Mix(base,hue,live[i]?10:5),220,0,0);
        Rounded(graphics,s.header,radius,UiTheme::Mix(panel,hue,live[i]?46:24),255,0,0,1.f,true);
        Rounded(graphics,s.rect,radius,0,0,selected?accent:UiTheme::Mix(UiTheme::Border(),hue,live[i]?55:25),255,selected?(std::max)(1.8f,2.4f*Zoom()):(std::max)(1.2f,1.5f*Zoom()));
        const bool show_remove=selected||hot;
        RECT title_rect{s.in.x+Px(12),s.header.top,(show_remove?s.remove.left:s.out.x-Px(10))-Px(4),s.header.bottom};
        Label(dc,CardTitle(i),title_rect,Scaled(UiTheme::Font::Strong),live[i]?text:muted,DT_SINGLELINE|DT_VCENTER|DT_LEFT|DT_END_ELLIPSIS);
        if (show_remove) {
            const bool remove_hot=hot_==Hit{Part::NodeRemove,index};
            Rounded(graphics,s.remove,static_cast<float>(Px(8)),remove_hot?UiTheme::Danger():UiTheme::Mix(panel,UiTheme::Danger(),35),255,0,0);
            Label(dc,L"×",s.remove,Scaled(UiTheme::Font::Small),remove_hot?RGB(255,255,255):text,DT_SINGLELINE|DT_VCENTER|DT_CENTER);
        }
        RECT label=Row(s.rect,0,26); label.right=s.minus.left-Px(6);
        Label(dc,Tr(L"多久后可接下一招"),label,Scaled(UiTheme::Font::Small),muted,DT_SINGLELINE|DT_VCENTER|DT_LEFT|DT_END_ELLIPSIS);
        const bool editing=edit_.node==index,caret=(GetTickCount64()/530)%2==0;
        const int cancel=NpcCombo::ClampCancel(node.ms);
        Stepper(dc,graphics,s,index,editing?edit_.text+(caret?L"▏":L" ")+L" s":cancel?Seconds(cancel/1000.0):Tr(L"放完"));
        if (editing) Rounded(graphics,s.value,static_cast<float>(Px(5)),0,0,accent,255,(std::max)(1.5f,2.f*Zoom()));
        if (NpcCombo::GroupSteps(config_,index).empty()) {
            RECT body{s.rect.left+Px(12),Row(s.rect,1).top,s.rect.right-Px(12),s.rect.bottom-Px(12)};
            Label(dc,NodeNote(i),body,Scaled(UiTheme::Font::Small),muted,DT_SINGLELINE|DT_VCENTER|DT_CENTER|DT_END_ELLIPSIS);
        }
        // The size handle: three short diagonal strokes in the corner.
        {
            const bool grip_hot=hot_==Hit{Part::GroupResize,index};
            Gdiplus::Pen grip(Argb(grip_hot?accent:UiTheme::Mix(muted,hue,30)),(std::max)(1.2f,1.6f*Zoom()));
            const float r=static_cast<float>(s.resize.right)-Px(4),b2=static_cast<float>(s.resize.bottom)-Px(4);
            for (int k=1;k<=3;++k) { const float d=static_cast<float>(Px(4*k)); graphics.DrawLine(&grip,r-d,b2,r,b2-d); }
        }
        ExecPin(graphics,s.in,fed[i],hot_==Hit{Part::NodeIn,index},live[i]?text:muted);
        ModPin(graphics,s.mod_out,modded[i],hot_==Hit{Part::ModOut,index},UiTheme::AccentBlue());
        ExecPin(graphics,s.out,node.next>=0,hot_==Hit{Part::NodeOut,index},live[i]?text:muted);
    }
    // Wires under the nodes: the played chains in the text colour, wires off them muted; arrows to modifiers straight,
    // in the modifier's colour.
    const float wire_w=(std::max)(1.5f,2.5f*Zoom());
    const auto arrows=[&](ArrowPart part) {
        for (size_t i=0;i<count;++i) for (int slot=0;slot<NpcCombo::kModSlots;++slot) {
            const int mod=NpcCombo::ModOf(config_,static_cast<int>(i),slot);
            if (mod<0||!shapes[i].has_mod_out) continue;
            const Shape& target=shapes[static_cast<size_t>(mod)];
            Arrow(graphics,shapes[i].mod_out,ArrowTip(shapes[i].mod_out,target.rect,target.mod_in),ModColor(config_.chain[static_cast<size_t>(mod)].type),live[i]?235:140,wire_w,part);
        }
    };
    arrows(ArrowPart::Shaft);
    for (size_t i=0;i<count;++i) {
        const Node& n=config_.chain[i];
        for (size_t o=0;o<NpcCombo::OutCount(n);++o) {
            const int next=NpcCombo::OutOf(n,o); if (next<0||static_cast<size_t>(next)>=count) continue;
            const POINT from=NpcCombo::HasRows(n)&&o<shapes[i].routes.size()?shapes[i].routes[o].out:shapes[i].out;
            const bool on=live[i]&&live[static_cast<size_t>(next)];
            Wire(graphics,from,shapes[static_cast<size_t>(next)].in,on?text:muted,on?230:150,wire_w);
        }
    }
    for (const size_t i:order) {
        const Node& node=config_.chain[i]; const Shape& s=shapes[i]; const int index=static_cast<int>(i);
        if (node.type==NodeType::Group) continue;  // drawn above, under everything
        const bool selected=IsPicked(index),playing=run_.running&&run_.profile==moves_.profile&&run_.node==index;
        const bool hot=hot_.index==index&&hot_.part>=Part::Node&&hot_.part<=Part::NodeRemove;
        const bool removable=!IsKey(index);
        switch (node.type) {
        case NodeType::Trigger: {
            const bool key=IsKey(index);
            Card(dc,graphics,s,TriggerColor(node.trigger),CardTitle(i),selected,false,!live[i],removable&&(selected||hot),hot_==Hit{Part::NodeRemove,index});
            if (key) KeyField(dc,s.field,text);
            else {
                const bool field_hot=hot_==Hit{Part::NodeField,index};
                Rounded(graphics,s.field,static_cast<float>(Px(5)),UiTheme::Mix(UiTheme::Base(),TriggerColor(node.trigger),field_hot?28:10),255,UiTheme::Mix(UiTheme::Border(),TriggerColor(node.trigger),30),255);
                RECT inner=s.field; InflateRect(&inner,-Px(7),0);
                Label(dc,Tr(NpcCombo::TriggerLabel(static_cast<Trigger>(node.trigger)))+L"  ▾",inner,Scaled(UiTheme::Font::Body),text,DT_SINGLELINE|DT_VCENTER|DT_LEFT|DT_END_ELLIPSIS);
            }
            RECT label=Row(s.rect,1); label.right=s.minus.left-Px(6);
            Label(dc,Tr(L"无按键多久后重置"),label,Scaled(UiTheme::Font::Small),muted,DT_SINGLELINE|DT_VCENTER|DT_LEFT|DT_END_ELLIPSIS);
            Stepper(dc,graphics,s,index,Seconds(NpcCombo::ClampReset(node.ms)/1000.0));
            if (!key) {
                const int box=s.toggle.bottom-s.toggle.top-Px(4);
                const RECT check{s.toggle.left,s.toggle.top+Px(2),s.toggle.left+box,s.toggle.top+Px(2)+box};
                const bool toggle_hot=hot_==Hit{Part::NodeToggle,index};
                Rounded(graphics,check,static_cast<float>(Px(4)),node.on?accent:UiTheme::Mix(UiTheme::Base(),text,toggle_hot?15:5),255,node.on?accent:UiTheme::Border(),255);
                if (node.on) {
                    Gdiplus::Pen tick(Argb(UiTheme::Base()),(std::max)(1.5f,2.f*Zoom()));
                    const float x=static_cast<float>(check.left),y=static_cast<float>(check.top),w=static_cast<float>(box);
                    Gdiplus::PointF points[3]={{x+w*.22f,y+w*.52f},{x+w*.42f,y+w*.72f},{x+w*.78f,y+w*.3f}};
                    graphics.DrawLines(&tick,points,3);
                }
                RECT word{check.right+Px(8),s.toggle.top,s.toggle.right,s.toggle.bottom};
                if (RawTrigger(node)) {
                    const int box2=s.toggle2.bottom-s.toggle2.top-Px(4);
                    const RECT check2{s.toggle2.left,s.toggle2.top+Px(2),s.toggle2.left+box2,s.toggle2.top+Px(2)+box2};
                    const bool swallow_hot=hot_==Hit{Part::NodeToggle2,index};
                    Rounded(graphics,check2,static_cast<float>(Px(4)),node.swallow?accent:UiTheme::Mix(UiTheme::Base(),text,swallow_hot?15:5),255,node.swallow?accent:UiTheme::Border(),255);
                    if (node.swallow) {
                        Gdiplus::Pen tick(Argb(UiTheme::Base()),(std::max)(1.5f,2.f*Zoom()));
                        const float x=static_cast<float>(check2.left),y=static_cast<float>(check2.top),w=static_cast<float>(box2);
                        Gdiplus::PointF points[3]={{x+w*.22f,y+w*.52f},{x+w*.42f,y+w*.72f},{x+w*.78f,y+w*.3f}};
                        graphics.DrawLines(&tick,points,3);
                    }
                    RECT word2{check2.right+Px(8),s.toggle2.top,s.toggle2.right,s.toggle2.bottom};
                    Label(dc,Tr(node.swallow?L"吞掉这些键原本的游戏动作":L"不吞掉：这些键照常做游戏动作"),word2,Scaled(UiTheme::Font::Small),node.swallow?text:muted,
                          DT_SINGLELINE|DT_VCENTER|DT_LEFT|DT_END_ELLIPSIS);
                }
                if (KeysTrigger(node)) {
                    const bool recording=record_.node==static_cast<int>(index)&&record_.route==-1;
                    const bool then_recording=record_.node==static_cast<int>(index)&&record_.route==kRecordThen;
                    const size_t least=RawTrigger(node)?1:NpcCombo::kComboKeysMin;
                    for (const auto& [rect,part,value,set]:{std::tuple<RECT,Part,std::wstring,bool>{s.field2,Part::NodeField2,std::wstring(),recording||node.keys.size()>=least},
                                                          std::tuple<RECT,Part,std::wstring,bool>{s.field3,Part::NodeField3,KeysModeText(node),true},
                                                          std::tuple<RECT,Part,std::wstring,bool>{s.field4,Part::NodeField4,ThenText(node,then_recording),true}}) {
                        const bool field_hot=hot_==Hit{part,index};
                        Rounded(graphics,rect,static_cast<float>(Px(5)),UiTheme::Mix(UiTheme::Base(),TriggerColor(node.trigger),field_hot?28:10),255,UiTheme::Mix(UiTheme::Border(),TriggerColor(node.trigger),30),255);
                        RECT inner=rect; InflateRect(&inner,-Px(7),0);
                        if (part==Part::NodeField2) KeysField(dc,inner,node.keys,node.chord,RawTrigger(node),recording,set,UiTheme::Font::Body,text,muted);  // the keys as icons
                        else Label(dc,value+L"  ▾",inner,Scaled(UiTheme::Font::Body),set?text:muted,DT_SINGLELINE|DT_VCENTER|DT_LEFT|DT_END_ELLIPSIS);
                    }
                }
                if (ExecutionTrigger(node)) {
                    const bool weapon_hot=hot_==Hit{Part::NodeField2,index};
                    Rounded(graphics,s.field2,static_cast<float>(Px(5)),UiTheme::Mix(UiTheme::Base(),WeaponColor(),weapon_hot?28:10),255,UiTheme::Mix(UiTheme::Border(),WeaponColor(),30),255);
                    RECT inner=s.field2; InflateRect(&inner,-Px(7),0);
                    Label(dc,ExecutionWeaponText(node)+L"  ▾",inner,Scaled(UiTheme::Font::Body),text,DT_SINGLELINE|DT_VCENTER|DT_LEFT|DT_END_ELLIPSIS);
                }
                Label(dc,Tr(!node.on?(KeysTrigger(node)?L"停用":L"停用：主角照常动作"):node.next<0?L"启用（还没连接招式）":
                            RawTrigger(node)?L"启用：按完这些键时放派生链":
                            KeysTrigger(node)?L"启用：按完组合键时取消主角动作，改放派生链":L"启用：取消主角动作，改放派生链"),word,Scaled(UiTheme::Font::Small),node.on?text:muted,
                      DT_SINGLELINE|DT_VCENTER|DT_LEFT|DT_END_ELLIPSIS);
            }
            break;
        }
        case NodeType::Interval:
            Card(dc,graphics,s,UiTheme::Mix(UiTheme::AccentBlue(),muted,40),CardTitle(i),selected,playing,!live[i],selected||hot,hot_==Hit{Part::NodeRemove,index});
            Stepper(dc,graphics,s,index,Seconds(node.ms/1000.0));
            Label(dc,NodeNote(i),Row(s.rect,1),Scaled(UiTheme::Font::Small),muted,DT_SINGLELINE|DT_VCENTER|DT_LEFT|DT_END_ELLIPSIS);
            break;
        case NodeType::PreInput:  // the chain's own setting: always in use, never removed
            Card(dc,graphics,s,UiTheme::Mix(UiTheme::Accent(),muted,30),CardTitle(i),selected,false,false,false,false);
            Stepper(dc,graphics,s,index,PreInputText(NpcCombo::ClampPreInput(node.ms)));
            Label(dc,NodeNote(i),Row(s.rect,1),Scaled(UiTheme::Font::Small),muted,DT_SINGLELINE|DT_VCENTER|DT_LEFT|DT_END_ELLIPSIS);
            break;
        case NodeType::WeaponActions: {
            // Its weapon, reset time and note, then a row per action: the action, x, its own output.
            const COLORREF hue=WeaponColor();
            Card(dc,graphics,s,hue,CardTitle(i),selected,false,!live[i],selected||hot,hot_==Hit{Part::NodeRemove,index});
            const bool field_hot=hot_==Hit{Part::NodeField,index};
            Rounded(graphics,s.field,static_cast<float>(Px(5)),UiTheme::Mix(UiTheme::Base(),hue,field_hot?28:10),255,UiTheme::Mix(UiTheme::Border(),hue,30),255);
            RECT inner=s.field; InflateRect(&inner,-Px(7),0);
            Label(dc,ActionsWeaponText(node)+L"  ▾",inner,Scaled(UiTheme::Font::Body),node.weapon.empty()?muted:text,DT_SINGLELINE|DT_VCENTER|DT_LEFT|DT_END_ELLIPSIS);
            RECT label=Row(s.rect,1); label.right=s.minus.left-Px(6);
            Label(dc,Tr(L"无按键多久后重置"),label,Scaled(UiTheme::Font::Small),muted,DT_SINGLELINE|DT_VCENTER|DT_LEFT|DT_END_ELLIPSIS);
            Stepper(dc,graphics,s,index,Seconds(NpcCombo::ClampReset(node.ms)/1000.0));
            Label(dc,NodeNote(i),Row(s.rect,2),Scaled(UiTheme::Font::Small),muted,DT_SINGLELINE|DT_VCENTER|DT_LEFT|DT_END_ELLIPSIS);
            for (size_t r=0;r<node.routes.size()&&r<s.routes.size();++r) {
                const NpcCombo::Route& route=node.routes[r]; const RouteShape& rs=s.routes[r]; const int row=static_cast<int>(r);
                const COLORREF row_hue=TriggerColor(route.button);
                const bool row_hot=hot_==Hit{Part::RouteField,index,row};
                Rounded(graphics,rs.field,static_cast<float>(Px(5)),UiTheme::Mix(UiTheme::Base(),row_hue,row_hot?28:10),255,UiTheme::Mix(UiTheme::Border(),row_hue,30),255);
                RECT row_inner=rs.field; InflateRect(&row_inner,-Px(6),0);
                Label(dc,ButtonLabel(route.button)+L" ▾",row_inner,Scaled(UiTheme::Font::Small),route.next>=0?text:muted,DT_SINGLELINE|DT_VCENTER|DT_LEFT|DT_END_ELLIPSIS);
                const bool remove_hot=hot_==Hit{Part::RouteRemove,index,row};
                Rounded(graphics,rs.remove,static_cast<float>(Px(5)),remove_hot?UiTheme::Danger():UiTheme::Mix(UiTheme::Base(),UiTheme::Danger(),12),255,UiTheme::Mix(UiTheme::Border(),UiTheme::Danger(),30),255);
                Label(dc,L"×",rs.remove,Scaled(UiTheme::Font::Small),remove_hot?RGB(255,255,255):text,DT_SINGLELINE|DT_VCENTER|DT_CENTER);
                ExecPin(graphics,rs.out,route.next>=0,hot_==Hit{Part::RouteOut,index,row},live[i]?text:muted);
            }
            const bool add_hot=hot_==Hit{Part::RouteAdd,index};
            Rounded(graphics,s.add,static_cast<float>(Px(5)),UiTheme::Mix(UiTheme::Base(),accent,add_hot?24:6),255,UiTheme::Mix(UiTheme::Border(),accent,30),255);
            Label(dc,Tr(L"+ 添加动作"),s.add,Scaled(UiTheme::Font::Small),text,DT_SINGLELINE|DT_VCENTER|DT_CENTER);
            break;
        }
        case NodeType::Branch: {
            Card(dc,graphics,s,BranchColor(),CardTitle(i),selected,false,!live[i],selected||hot,hot_==Hit{Part::NodeRemove,index});
            Label(dc,NodeNote(i),Row(s.rect,0),Scaled(UiTheme::Font::Small),muted,DT_SINGLELINE|DT_VCENTER|DT_LEFT|DT_END_ELLIPSIS);
            for (size_t r=0;r<node.routes.size()&&r<s.routes.size();++r) {
                const NpcCombo::Route& route=node.routes[r]; const RouteShape& rs=s.routes[r]; const int row=static_cast<int>(r);
                const COLORREF hue=TriggerColor(route.button);
                const bool field_hot=hot_==Hit{Part::RouteField,index,row};
                Rounded(graphics,rs.field,static_cast<float>(Px(5)),UiTheme::Mix(UiTheme::Base(),hue,field_hot?28:10),255,UiTheme::Mix(UiTheme::Border(),hue,30),255);
                RECT inner=rs.field; InflateRect(&inner,-Px(6),0);
                Label(dc,ButtonLabel(route.button)+L" ▾",inner,Scaled(UiTheme::Font::Small),text,DT_SINGLELINE|DT_VCENTER|DT_LEFT|DT_END_ELLIPSIS);
                StepperAt(dc,graphics,rs.minus,rs.value,rs.plus,PauseText(route.pause_ms),Hit{Part::RouteMinus,index,row},Hit{Part::RoutePlus,index,row},Hit{Part::RouteValue,index,row},UiTheme::Font::Small);
                const bool remove_hot=hot_==Hit{Part::RouteRemove,index,row};
                Rounded(graphics,rs.remove,static_cast<float>(Px(5)),remove_hot?UiTheme::Danger():UiTheme::Mix(UiTheme::Base(),UiTheme::Danger(),12),255,UiTheme::Mix(UiTheme::Border(),UiTheme::Danger(),30),255);
                Label(dc,L"×",rs.remove,Scaled(UiTheme::Font::Small),remove_hot?RGB(255,255,255):text,DT_SINGLELINE|DT_VCENTER|DT_CENTER);
                ExecPin(graphics,rs.out,route.next>=0,hot_==Hit{Part::RouteOut,index,row},live[i]?text:muted);
                if (rs.has_keys) {
                    // Its keys (recording: what it has so far) and how they are pressed.
                    const bool raw=RawRoute(route),recording=record_.node==static_cast<int>(index)&&record_.route==row;
                    for (const auto& [rect,part,value,set]:{std::tuple<RECT,Part,std::wstring,bool>{rs.keys,Part::RouteKeys,std::wstring(),recording||NpcCombo::KeysRouteReady(route)},
                                                          std::tuple<RECT,Part,std::wstring,bool>{rs.mode,Part::RouteMode,RouteModeText(route),true}}) {
                        const bool part_hot=hot_==Hit{part,index,row};
                        Rounded(graphics,rect,static_cast<float>(Px(5)),UiTheme::Mix(UiTheme::Base(),hue,part_hot?28:10),255,UiTheme::Mix(UiTheme::Border(),hue,30),255);
                        RECT inner2=rect; InflateRect(&inner2,-Px(6),0);
                        if (part==Part::RouteKeys) KeysField(dc,inner2,route.keys,route.chord,raw,recording,set,UiTheme::Font::Small,text,muted);  // the keys as icons
                        else Label(dc,value+L" ▾",inner2,Scaled(UiTheme::Font::Small),set?text:muted,DT_SINGLELINE|DT_VCENTER|DT_LEFT|DT_END_ELLIPSIS);
                    }
                }
            }
            const bool add_hot=hot_==Hit{Part::RouteAdd,index};
            Rounded(graphics,s.add,static_cast<float>(Px(5)),UiTheme::Mix(UiTheme::Base(),accent,add_hot?24:6),255,UiTheme::Mix(UiTheme::Border(),accent,30),255);
            Label(dc,Tr(L"+ 添加按键"),s.add,Scaled(UiTheme::Font::Small),text,DT_SINGLELINE|DT_VCENTER|DT_CENTER);
            break;
        }
        case NodeType::Speed: {
            Card(dc,graphics,s,ModColor(node.type),CardTitle(i),selected,false,!pointed[i],selected||hot,hot_==Hit{Part::NodeRemove,index});
            const bool editing=edit_.node==index;
            const bool caret=(GetTickCount64()/530)%2==0;
            Stepper(dc,graphics,s,index,editing?L"×"+edit_.text+(caret?L"▏":L" "):SpeedText(NpcCombo::ClampSpeed(node.speed)));
            if (editing) Rounded(graphics,s.value,static_cast<float>(Px(5)),0,0,accent,255,(std::max)(1.5f,2.f*Zoom()));
            Label(dc,NodeNote(i),Row(s.rect,1),Scaled(UiTheme::Font::Small),muted,DT_SINGLELINE|DT_VCENTER|DT_LEFT|DT_END_ELLIPSIS);
            break;
        }
        case NodeType::Damage: {
            Card(dc,graphics,s,ModColor(node.type),CardTitle(i),selected,false,!pointed[i],selected||hot,hot_==Hit{Part::NodeRemove,index});
            const bool editing=edit_.node==index;
            const bool caret=(GetTickCount64()/530)%2==0;
            Stepper(dc,graphics,s,index,editing?edit_.text+(caret?L"▏":L" ")+L"%":DamageText(NpcCombo::ClampDamage(node.speed)));
            if (editing) Rounded(graphics,s.value,static_cast<float>(Px(5)),0,0,accent,255,(std::max)(1.5f,2.f*Zoom()));
            Label(dc,NodeNote(i),Row(s.rect,1),Scaled(UiTheme::Font::Small),muted,DT_SINGLELINE|DT_VCENTER|DT_LEFT|DT_END_ELLIPSIS);
            break;
        }
        case NodeType::WeaponMove: {
            Card(dc,graphics,s,WeaponColor(),CardTitle(i),selected,playing,!live[i],selected||hot,hot_==Hit{Part::NodeRemove,index});
            const auto field=[&](const RECT& r,Part part,const std::wstring& label,bool on){
                const bool field_hot=hot_==Hit{part,index};
                Rounded(graphics,r,static_cast<float>(Px(5)),UiTheme::Mix(UiTheme::Base(),WeaponColor(),field_hot?28:10),255,UiTheme::Mix(UiTheme::Border(),WeaponColor(),30),255);
                RECT inner=r; InflateRect(&inner,-Px(7),0);
                Label(dc,label+L"  ▾",inner,Scaled(UiTheme::Font::Body),on?text:muted,DT_SINGLELINE|DT_VCENTER|DT_LEFT|DT_END_ELLIPSIS);
            };
            field(s.field,Part::NodeField,node.weapon.empty()?Tr(L"选择武器"):WeaponName(node.weapon),!node.weapon.empty());
            field(s.field2,Part::NodeField2,node.ability.empty()?Tr(L"选择招式"):WeaponMoveText(node.weapon,node.ability),!node.ability.empty());
            Label(dc,NodeNote(i),Row(s.rect,2,26),Scaled(UiTheme::Font::Small),muted,DT_SINGLELINE|DT_VCENTER|DT_LEFT|DT_END_ELLIPSIS);
            RECT label=Row(s.rect,3); label.right=s.minus.left-Px(6);
            Label(dc,Tr(L"多久后可接下一招"),label,Scaled(UiTheme::Font::Small),muted,DT_SINGLELINE|DT_VCENTER|DT_LEFT|DT_END_ELLIPSIS);
            const bool editing=edit_.node==index,caret=(GetTickCount64()/530)%2==0;
            const int cancel=NpcCombo::ClampCancel(node.ms);
            Stepper(dc,graphics,s,index,editing?edit_.text+(caret?L"▏":L" ")+L" s":cancel?Seconds(cancel/1000.0):Tr(L"放完"));
            if (editing) Rounded(graphics,s.value,static_cast<float>(Px(5)),0,0,accent,255,(std::max)(1.5f,2.f*Zoom()));
            break;
        }
        case NodeType::Teleport: {
            Card(dc,graphics,s,TeleportColor(),CardTitle(i),selected,playing,!live[i],selected||hot,hot_==Hit{Part::NodeRemove,index});
            const auto field=[&](const RECT& r,Part part,const std::wstring& label){
                const bool field_hot=hot_==Hit{part,index};
                Rounded(graphics,r,static_cast<float>(Px(5)),UiTheme::Mix(UiTheme::Base(),TeleportColor(),field_hot?28:10),255,UiTheme::Mix(UiTheme::Border(),TeleportColor(),30),255);
                RECT inner=r; InflateRect(&inner,-Px(7),0);
                Label(dc,label+L"  ▾",inner,Scaled(UiTheme::Font::Body),text,DT_SINGLELINE|DT_VCENTER|DT_LEFT|DT_END_ELLIPSIS);
            };
            field(s.field,Part::NodeField,DistText(NpcCombo::ClampTeleportDist(node.dist)));
            field(s.field2,Part::NodeField2,LiftText(NpcCombo::ClampTeleportLift(node.lift)));
            Label(dc,NodeNote(i),Row(s.rect,2,26),Scaled(UiTheme::Font::Small),muted,DT_SINGLELINE|DT_VCENTER|DT_LEFT|DT_END_ELLIPSIS);
            RECT label=Row(s.rect,3); label.right=s.minus.left-Px(6);
            Label(dc,Tr(L"多久后可接下一招"),label,Scaled(UiTheme::Font::Small),muted,DT_SINGLELINE|DT_VCENTER|DT_LEFT|DT_END_ELLIPSIS);
            const bool editing=edit_.node==index,caret=(GetTickCount64()/530)%2==0;
            const int cancel=NpcCombo::ClampCancel(node.ms);
            Stepper(dc,graphics,s,index,editing?edit_.text+(caret?L"▏":L" ")+L" s":cancel?Seconds(cancel/1000.0):Tr(L"放完"));
            if (editing) Rounded(graphics,s.value,static_cast<float>(Px(5)),0,0,accent,255,(std::max)(1.5f,2.f*Zoom()));
            break;
        }
        case NodeType::BossSkill: {
            Card(dc,graphics,s,BossColor(),CardTitle(i),selected,playing,!live[i],selected||hot,hot_==Hit{Part::NodeRemove,index});
            const auto field=[&](const RECT& r,Part part,const std::wstring& label,bool on){
                const bool field_hot=hot_==Hit{part,index};
                Rounded(graphics,r,static_cast<float>(Px(5)),UiTheme::Mix(UiTheme::Base(),BossColor(),field_hot?28:10),255,UiTheme::Mix(UiTheme::Border(),BossColor(),30),255);
                RECT inner=r; InflateRect(&inner,-Px(7),0);
                Label(dc,label+L"  ▾",inner,Scaled(UiTheme::Font::Body),on?text:muted,DT_SINGLELINE|DT_VCENTER|DT_LEFT|DT_END_ELLIPSIS);
            };
            field(s.field,Part::NodeField,node.boss.empty()?Tr(L"选择外观"):BossLookName(node.boss),!node.boss.empty());
            field(s.field2,Part::NodeField2,node.ability.empty()?Tr(L"选择招式"):MoveText(node.ability),!node.ability.empty());
            RECT label=Row(s.rect,2); label.right=s.minus.left-Px(6);
            Label(dc,Tr(L"多久后可接下一招"),label,Scaled(UiTheme::Font::Small),muted,DT_SINGLELINE|DT_VCENTER|DT_LEFT|DT_END_ELLIPSIS);
            const bool editing=edit_.node==index,caret=(GetTickCount64()/530)%2==0;
            const int cancel=NpcCombo::ClampCancel(node.ms);
            Stepper(dc,graphics,s,index,editing?edit_.text+(caret?L"▏":L" ")+L" s":cancel?Seconds(cancel/1000.0):Tr(L"放完"));
            if (editing) Rounded(graphics,s.value,static_cast<float>(Px(5)),0,0,accent,255,(std::max)(1.5f,2.f*Zoom()));
            break;
        }
        case NodeType::Look: {
            Card(dc,graphics,s,BossColor(),CardTitle(i),selected,playing,!live[i],selected||hot,hot_==Hit{Part::NodeRemove,index});
            const bool field_hot=hot_==Hit{Part::NodeField,index};
            Rounded(graphics,s.field,static_cast<float>(Px(5)),UiTheme::Mix(UiTheme::Base(),BossColor(),field_hot?28:10),255,UiTheme::Mix(UiTheme::Border(),BossColor(),30),255);
            RECT inner=s.field; InflateRect(&inner,-Px(7),0);
            Label(dc,(node.boss.empty()?Tr(L"选择外观"):BossLookName(node.boss))+L"  ▾",inner,Scaled(UiTheme::Font::Body),node.boss.empty()?muted:text,
                  DT_SINGLELINE|DT_VCENTER|DT_LEFT|DT_END_ELLIPSIS);
            Label(dc,NodeNote(i),Row(s.rect,1),Scaled(UiTheme::Font::Small),muted,DT_SINGLELINE|DT_VCENTER|DT_LEFT|DT_END_ELLIPSIS);
            break;
        }
        case NodeType::ForceHit: {
            Card(dc,graphics,s,ModColor(node.type),CardTitle(i),selected,false,!pointed[i],selected||hot,hot_==Hit{Part::NodeRemove,index});
            const bool field_hot=hot_==Hit{Part::NodeField,index};
            Rounded(graphics,s.field,static_cast<float>(Px(5)),UiTheme::Mix(UiTheme::Base(),ModColor(node.type),field_hot?28:10),255,UiTheme::Mix(UiTheme::Border(),ModColor(node.type),30),255);
            RECT inner=s.field; InflateRect(&inner,-Px(7),0);
            Label(dc,Tr(NpcCombo::HitKindLabel(node.hit))+L"  ▾",inner,Scaled(UiTheme::Font::Body),text,DT_SINGLELINE|DT_VCENTER|DT_LEFT|DT_END_ELLIPSIS);
            Label(dc,NodeNote(i),Row(s.rect,1),Scaled(UiTheme::Font::Small),pointed[i]?text:muted,DT_SINGLELINE|DT_VCENTER|DT_LEFT|DT_END_ELLIPSIS);
            break;
        }
        case NodeType::HeroLook:
            Card(dc,graphics,s,TeleportColor(),CardTitle(i),selected,playing,!live[i],selected||hot,hot_==Hit{Part::NodeRemove,index});
            Label(dc,NodeNote(i),Row(s.rect,0),Scaled(UiTheme::Font::Small),muted,DT_SINGLELINE|DT_VCENTER|DT_LEFT|DT_END_ELLIPSIS);
            break;
        case NodeType::Grab:
            Card(dc,graphics,s,GrabColor(),CardTitle(i),selected,playing,!live[i],selected||hot,hot_==Hit{Part::NodeRemove,index});
            {
                // The field opens the list of grabs.
                const bool field_hot=hot_==Hit{Part::NodeField,index};
                Rounded(graphics,s.field,static_cast<float>(Px(5)),UiTheme::Mix(UiTheme::Base(),GrabColor(),field_hot?28:10),255,UiTheme::Mix(UiTheme::Border(),GrabColor(),30),255);
                RECT inner=s.field; InflateRect(&inner,-Px(7),0);
                Label(dc,GrabField(node)+L"  ▾",inner,Scaled(UiTheme::Font::Body),NodeGrab(node)?text:muted,DT_SINGLELINE|DT_VCENTER|DT_LEFT|DT_END_ELLIPSIS);
            }
            Label(dc,NodeNote(i),Row(s.rect,1),Scaled(UiTheme::Font::Small),muted,DT_SINGLELINE|DT_VCENTER|DT_LEFT|DT_END_ELLIPSIS);
            break;
        case NodeType::Armor: case NodeType::Interrupt: case NodeType::Invincible: case NodeType::KeepLook:
            Card(dc,graphics,s,ModColor(node.type),CardTitle(i),selected,false,!pointed[i],selected||hot,hot_==Hit{Part::NodeRemove,index});
            Label(dc,NodeNote(i),Row(s.rect,0),Scaled(UiTheme::Font::Small),pointed[i]?text:muted,DT_SINGLELINE|DT_VCENTER|DT_LEFT|DT_END_ELLIPSIS);
            break;
        case NodeType::Weapon:
            Card(dc,graphics,s,ModColor(node.type),CardTitle(i),selected,false,!pointed[i],selected||hot,hot_==Hit{Part::NodeRemove,index});
            Label(dc,NodeNote(i),Row(s.rect,0),Scaled(UiTheme::Font::Small),pointed[i]?text:muted,DT_SINGLELINE|DT_VCENTER|DT_LEFT|DT_END_ELLIPSIS);
            Label(dc,WeaponNote(),Row(s.rect,1),Scaled(UiTheme::Font::Small),muted,DT_SINGLELINE|DT_VCENTER|DT_LEFT|DT_END_ELLIPSIS);
            break;
        default: {
            const COLORREF hue=node.type==NodeType::AutoDodge?KindColor(Kind::Dodge):KindColor(KindOf(node.ability));
            Card(dc,graphics,s,hue,CardTitle(i),selected,playing,!live[i],selected||hot,hot_==Hit{Part::NodeRemove,index});
            Label(dc,NodeNote(i),Row(s.rect,0,26),Scaled(UiTheme::Font::Small),muted,DT_SINGLELINE|DT_VCENTER|DT_LEFT|DT_END_ELLIPSIS);
            // Its 派生点: from how long after it starts the next move may cut it (a press before is kept for then).
            RECT label=Row(s.rect,1); label.right=s.minus.left-Px(6);
            Label(dc,Tr(L"多久后可接下一招"),label,Scaled(UiTheme::Font::Small),muted,DT_SINGLELINE|DT_VCENTER|DT_LEFT|DT_END_ELLIPSIS);
            const bool editing=edit_.node==index,caret=(GetTickCount64()/530)%2==0;
            const int cancel=NpcCombo::ClampCancel(node.ms);
            Stepper(dc,graphics,s,index,editing?edit_.text+(caret?L"▏":L" ")+L" s":cancel?Seconds(cancel/1000.0):Tr(L"放完"));
            if (editing) Rounded(graphics,s.value,static_cast<float>(Px(5)),0,0,accent,255,(std::max)(1.5f,2.f*Zoom()));
            break;
        }
        }
        if (s.has_in) ExecPin(graphics,s.in,fed[i],hot_==Hit{Part::NodeIn,index},live[i]?text:muted);
        if (s.has_mod_out) ModPin(graphics,s.mod_out,modded[i],hot_==Hit{Part::ModOut,index},UiTheme::AccentBlue());
        if (s.has_mod_in) ModPin(graphics,s.mod_in,pointed[i],hot_==Hit{Part::ModIn,index},ModColor(node.type));
        if (s.has_out) ExecPin(graphics,s.out,node.next>=0,hot_==Hit{Part::NodeOut,index},live[i]?text:muted);
    }
    // Arrow heads over every card: an arrow never ends under a node.
    arrows(ArrowPart::Head);
    // The wire or arrow being drawn.
    if ((drag_.mode==Drag::Wire||drag_.mode==Drag::WireBack||drag_.mode==Drag::Arrow||drag_.mode==Drag::ArrowBack)&&drag_.moved&&At(drag_.index)) {
        const Shape& s=shapes[static_cast<size_t>(drag_.index)];
        if (drag_.mode==Drag::Wire) {
            const POINT from=drag_.sub>=0&&static_cast<size_t>(drag_.sub)<s.routes.size()?s.routes[static_cast<size_t>(drag_.sub)].out:s.out;
            Wire(graphics,from,cursor_,accent,255,wire_w);
        }
        else if (drag_.mode==Drag::WireBack) Wire(graphics,cursor_,s.in,accent,255,wire_w);
        else if (drag_.mode==Drag::Arrow) Arrow(graphics,s.mod_out,cursor_,UiTheme::AccentBlue(),255,wire_w);
        else Arrow(graphics,cursor_,ArrowTip(cursor_,s.rect,s.mod_in),ModColor(config_.chain[static_cast<size_t>(drag_.index)].type),255,wire_w);
    }
    if (config_.chain.size()<=1) {
        const int key=NpcCombo::KeyTrigger(config_);
        if (key>=0) {
            const RECT& k=shapes[static_cast<size_t>(key)].rect;
            RECT empty{k.right+m.S(24),k.top,canvas_.right-m.S(16),k.bottom};
            Label(dc,Tr(L"点上方招式接到「按键」后面，或把招式拖进画布；Tab / 右键搜索。"),empty,Font(UiTheme::Font::Small),muted,DT_SINGLELINE|DT_VCENTER|DT_LEFT|DT_END_ELLIPSIS);
        }
    }
    if (drag_.mode==Drag::Box&&drag_.moved) {
        const RECT box=BoxRect();
        Rounded(graphics,box,static_cast<float>(m.S(3)),UiTheme::Accent(),36,UiTheme::Accent(),230,(std::max)(1.f,1.5f*Zoom()));
    }
    wchar_t zoom[32]{}; _snwprintf_s(zoom,_countof(zoom),_TRUNCATE,L"%s %d%%",Tr(L"缩放").c_str(),static_cast<int>(std::lround(Zoom()*100)));
    RECT corner{canvas_.left,canvas_.top+m.S(8),canvas_.right-m.S(14),canvas_.top+m.S(30)};
    Label(dc,zoom,corner,Font(UiTheme::Font::Small),muted,DT_SINGLELINE|DT_VCENTER|DT_RIGHT);
    graphics.ResetClip();
    RestoreDC(dc,saved);
    Rounded(graphics,canvas_,static_cast<float>(m.S(10)),0,0,UiTheme::Mix(border,base,20),255);
}
void Paint(HDC printed=nullptr) {
    // Laid out and framed on the first paint, when the page has its real size (not the size it was created at).
    if (HasSkin()&&canvas_.right>canvas_.left) {
        if (!config_.placed) { Arrange(); framed_=false; }
        if (!framed_) { Frame(false); framed_=true; }
    }
    Buffered(window_,printed,[&](HDC dc,Gdiplus::Graphics& graphics,const RECT& client) {
        (void)client;
        const auto m=M(); const COLORREF paper=Paper(),accent=UiTheme::Accent(),text=UiTheme::Text(),muted=UiTheme::MutedText();
        const HFONT body=Font(UiTheme::Font::Body),strong=Font(UiTheme::Font::Strong),small_font=Font(UiTheme::Font::Small);
        if (hot_.part==Part::Header||(search_.open&&search_.mode==3))
            Rounded(graphics,header_,static_cast<float>(m.S(8)),UiTheme::Mix(paper,accent,hot_.part==Part::Header?18:10),255,UiTheme::Mix(UiTheme::Border(),accent,40),255);
        RECT title=header_; title.left+=m.S(8);
        Label(dc,HeaderText(),title,HasSkin()?strong:body,HasSkin()?text:muted,DT_SINGLELINE|DT_VCENTER|DT_LEFT|DT_END_ELLIPSIS);
        DrawButton(dc,graphics,Button::Toggle,config_.enabled,HasSkin());
        DrawButton(dc,graphics,Button::Toast,NpcCombo::ShowToast(),true);
        DrawButton(dc,graphics,Button::Dummy,dummy_present_,true);
        Label(dc,Tr(L"Boss 招式"),tabs_title_,strong,text,DT_SINGLELINE|DT_VCENTER|DT_LEFT);
        for (size_t i=0;i<tabs_.size();++i) {
            const Tab& tab=tabs_[i]; const bool on=tab.kind==category_,hot=hot_==Hit{Part::Tab,static_cast<int>(i)};
            const COLORREF hue=tab.kind<0?accent:KindColor(static_cast<Kind>(tab.kind));
            Rounded(graphics,tab.rect,static_cast<float>(m.S(14)),UiTheme::Mix(paper,hue,on?38:hot?18:6),255,on?hue:UiTheme::Mix(UiTheme::Border(),paper,30),255,on?1.5f:1.f);
            Label(dc,TabText(tab),tab.rect,on?Font(UiTheme::Font::Strong):small_font,on?text:UiTheme::Mix(hue,text,35),DT_SINGLELINE|DT_VCENTER|DT_CENTER);
        }
        if (!HasSkin()) Label(dc,Tr(L"没有外观，没有可列出的招式。"),strip_,small_font,muted,DT_SINGLELINE|DT_VCENTER|DT_LEFT);
        {
            const int saved=SaveDC(dc);
            IntersectClipRect(dc,strip_area_.left,strip_area_.top,strip_area_.right,strip_area_.bottom);
            graphics.SetClip(Gdiplus::Rect(strip_area_.left,strip_area_.top,strip_area_.right-strip_area_.left,strip_area_.bottom-strip_area_.top));
            for (size_t i=0;i<pills_.size();++i) {
                const Pill& pill=pills_[i];
                if (pill.rect.right<strip_area_.left||pill.rect.left>strip_area_.right) continue;
                const bool hot=hot_==Hit{Part::Pill,static_cast<int>(i)};
                const COLORREF hue=KindColor(pill.kind);
                Rounded(graphics,pill.rect,static_cast<float>(m.S(15)),UiTheme::Mix(paper,hue,hot?30:14),255,UiTheme::Mix(paper,hue,hot?90:55),255);
                RECT inner=pill.rect; InflateRect(&inner,-m.S(10),0);
                Label(dc,MoveText(pill.ability),inner,body,text,DT_SINGLELINE|DT_VCENTER|DT_CENTER|DT_END_ELLIPSIS);
            }
            graphics.ResetClip(); RestoreDC(dc,saved);
        }
        for (const auto& [rect,glyph,part]:{std::tuple{strip_left_,L"◀",Part::StripLeft},std::tuple{strip_right_,L"▶",Part::StripRight}}) {
            if (rect.right<=rect.left) continue;
            Rounded(graphics,rect,static_cast<float>(m.S(8)),UiTheme::Mix(paper,UiTheme::Base(),hot_.part==part?20:45),255,UiTheme::Mix(UiTheme::Border(),paper,35),255);
            Label(dc,glyph,rect,small_font,text,DT_SINGLELINE|DT_VCENTER|DT_CENTER);
        }
        if (HasSkin()) DrawCanvas(dc,graphics);
        else {
            Rounded(graphics,canvas_,static_cast<float>(m.S(10)),UiTheme::Base(),255,UiTheme::Border(),255);
            Label(dc,Tr(L"套用外观后可以编排。"),canvas_,small_font,muted,DT_SINGLELINE|DT_VCENTER|DT_CENTER);
        }
        if (HasSkin()&&weapons_.right>weapons_.left) {
            // The weapon list.
            Rounded(graphics,weapons_,static_cast<float>(m.S(10)),UiTheme::Base(),255,UiTheme::Mix(UiTheme::Border(),paper,20),255);
            Label(dc,Tr(L"武器 · 拖到画布"),weapons_title_,strong,text,DT_SINGLELINE|DT_VCENTER|DT_LEFT|DT_END_ELLIPSIS);
            const int saved=SaveDC(dc);
            const int top=weapons_title_.bottom+m.S(4);
            IntersectClipRect(dc,weapons_.left,top,weapons_.right,weapons_.bottom-m.S(4));
            graphics.SetClip(Gdiplus::Rect(weapons_.left,top,weapons_.right-weapons_.left,weapons_.bottom-m.S(4)-top));
            for (size_t i=0;i<weapon_rows_.size();++i) {
                const WeaponRow& row=weapon_rows_[i];
                if (row.rect.bottom<top||row.rect.top>weapons_.bottom) continue;
                if (row.header) { Label(dc,row.name,row.rect,small_font,muted,DT_SINGLELINE|DT_BOTTOM|DT_LEFT); continue; }
                const bool hot=hot_==Hit{Part::WeaponRow,static_cast<int>(i)};
                if (hot) Rounded(graphics,row.rect,static_cast<float>(m.S(6)),UiTheme::Mix(UiTheme::Base(),WeaponColor(),22),255,UiTheme::Mix(UiTheme::Border(),WeaponColor(),40),255);
                const int icon_w=m.S(46);
                WeaponIcons::Draw(graphics,row.image,Gdiplus::RectF(static_cast<float>(row.rect.left+m.S(4)),static_cast<float>(row.rect.top+m.S(3)),
                                                                     static_cast<float>(icon_w),static_cast<float>(row.rect.bottom-row.rect.top-m.S(6))));
                RECT name{row.rect.left+icon_w+m.S(10),row.rect.top,row.rect.right-m.S(4),row.rect.bottom};
                Label(dc,row.name,name,small_font,text,DT_SINGLELINE|DT_VCENTER|DT_LEFT|DT_END_ELLIPSIS);
            }
            graphics.ResetClip(); RestoreDC(dc,saved);
        }
        const bool any=At(selected_)&&!IsKey(selected_)&&!IsPreInputAt(selected_);
        if (HasSkin()) for (const Button b:kTools) DrawButton(dc,graphics,b,false,b==Button::AutoDodge?!moves_.dodges.empty():b!=Button::Remove||any);
        if (search_.open) DrawSearch(dc,graphics);
        // A move being dragged from the strip.
        if (drag_.mode==Drag::Pill&&drag_.moved&&drag_.index>=0&&static_cast<size_t>(drag_.index)<pills_.size()) {
            const Pill& pill=pills_[static_cast<size_t>(drag_.index)];
            RECT ghost=pill.rect; OffsetRect(&ghost,cursor_.x-(pill.rect.left+pill.rect.right)/2,cursor_.y-(pill.rect.top+pill.rect.bottom)/2);
            Rounded(graphics,ghost,static_cast<float>(m.S(15)),UiTheme::Mix(paper,KindColor(pill.kind),40),220,accent,255,1.5f);
            RECT inner=ghost; InflateRect(&inner,-m.S(10),0);
            Label(dc,MoveText(pill.ability),inner,body,text,DT_SINGLELINE|DT_VCENTER|DT_CENTER|DT_END_ELLIPSIS);
        }
        // A weapon being dragged from the list.
        if (drag_.mode==Drag::WeaponRow&&drag_.moved&&drag_.index>=0&&static_cast<size_t>(drag_.index)<weapon_rows_.size()) {
            const WeaponRow& row=weapon_rows_[static_cast<size_t>(drag_.index)];
            RECT ghost=row.rect; OffsetRect(&ghost,cursor_.x-(row.rect.left+row.rect.right)/2,cursor_.y-(row.rect.top+row.rect.bottom)/2);
            Rounded(graphics,ghost,static_cast<float>(m.S(8)),UiTheme::Mix(paper,WeaponColor(),40),220,accent,255,1.5f);
            RECT inner=ghost; InflateRect(&inner,-m.S(10),0);
            Label(dc,row.name,inner,body,text,DT_SINGLELINE|DT_VCENTER|DT_CENTER|DT_END_ELLIPSIS);
        }
        // Status line: what the runner said, then the canvas controls.
        const std::wstring note=run_.profile==moves_.profile?Tr(run_.note.c_str()):std::wstring();
        int used=0;
        if (!note.empty()) {
            used=(std::min)(static_cast<int>(Extent(dc,small_font,note).cx)+m.S(24),static_cast<int>(footer_.right-footer_.left)/2);
            Label(dc,note,{footer_.left,footer_.top,footer_.left+used,footer_.bottom},small_font,run_.running?accent:muted,DT_SINGLELINE|DT_VCENTER|DT_LEFT|DT_END_ELLIPSIS);
        }
        Label(dc,hint_text_,{footer_.left+used,footer_.top,footer_.right,footer_.bottom},small_font,muted,DT_SINGLELINE|DT_VCENTER|DT_RIGHT|DT_END_ELLIPSIS);
    });
}

// ---- input ---------------------------------------------------------------------------------------
// The applied skin, its moves and its saved chains; a new skin starts with nothing selected and the view framed.
void Refresh(bool force) {
    const NpcRetarget::Snapshot snapshot=NpcRetarget::GetSnapshot();
    const NpcCombo::Files files=NpcCombo::GetFiles();
    if (force||files.serial!=files_serial_) {
        files_serial_=files.serial; files_=files;
        if (!files.status.empty()&&files.status!=file_status_) DebugHub::Notify(StatusText(files.status).c_str());
        file_status_=files.status; Layout();
    }
    // Whose chain: the look's, the hero's own (-1), or while a BOSS 技能 look is borrowed the look before it; or the
    // one picked on the header.
    if (snapshot.chain!=game_chain_) { game_chain_=snapshot.chain; Layout(); }
    const int32_t want=pinned_==kFollowGame?snapshot.chain:pinned_;
    if (force||want!=skin_) {
        skin_=want; Select(-1); drag_={}; search_.open=false; edit_={}; picked_.clear();
        if (!NpcRetarget::GetSkinMoves(skin_,moves_)) moves_={};
        config_=HasSkin()?NpcCombo::Load(moves_.profile):NpcCombo::Config{};
        seen_generation_=NpcCombo::Generation(); framed_=false; Layout();
    } else if (HasSkin()&&NpcCombo::Generation()!=seen_generation_) {
        // The file changed (another file picked, or edited on disk): the chain as it is now.
        seen_generation_=NpcCombo::Generation(); config_=NpcCombo::Load(moves_.profile); Select(-1); Layout();
    }
    if (skin_==-1&&GetTickCount64()>=hero_check_) {
        // The hero's moves are read in game every few seconds.
        hero_check_=GetTickCount64()+2000; NpcRetarget::SkinMoves fresh;
        if (NpcRetarget::GetSkinMoves(-1,fresh)&&fresh.skills!=moves_.skills) { moves_=fresh; Layout(); }
    }
    const NpcCombo::Run run=NpcCombo::GetRun();
    if (run.serial!=run_serial_) { run_serial_=run.serial; run_=run; Invalidate(); }
    const uint64_t weapon=NpcCombo::LastWeapon().serial;
    if (weapon!=weapon_serial_) { weapon_serial_=weapon; Invalidate(); }
}
void BeginDrag(Drag mode,const Hit& hit,POINT p,int index,bool right=false) {
    drag_={}; drag_.mode=mode; drag_.hit=hit; drag_.start=p; drag_.last=p; drag_.index=index; drag_.right=right; SetCapture(window_);
}
bool Moved(POINT p) { return std::abs(p.x-drag_.start.x)>M().S(4)||std::abs(p.y-drag_.start.y)>M().S(4); }
// The nodes a dragged card moves: the selection when it is one of them, else itself.
std::vector<int> DragMovers() {
    std::vector<int> movers{drag_.index};
    if (picked_.size()>1&&IsPicked(drag_.index)) for (const int i:Selection()) if (i!=drag_.index) movers.push_back(i);
    return movers;
}
void DragMove(POINT p) {
    cursor_=p;
    if (drag_.mode==Drag::None) return;
    if (!drag_.moved&&!Moved(p)) return;
    drag_.moved=true;
    // Whole canvas units moved since the press, so slow drags are not lost to rounding.
    const auto shift=[&](int& x,int& y){
        const int ux=static_cast<int>(std::lround((p.x-drag_.start.x)/Scale())),uy=static_cast<int>(std::lround((p.y-drag_.start.y)/Scale()));
        const int lx=static_cast<int>(std::lround((drag_.last.x-drag_.start.x)/Scale())),ly=static_cast<int>(std::lround((drag_.last.y-drag_.start.y)/Scale()));
        x=NpcCombo::ClampPosition(x+ux-lx); y=NpcCombo::ClampPosition(y+uy-ly);
    };
    if (drag_.mode==Drag::Node&&At(drag_.index)) {
        Node& moved=config_.chain[static_cast<size_t>(drag_.index)];
        const int x0=moved.x,y0=moved.y;
        shift(moved.x,moved.y);
        const int dx=moved.x-x0,dy=moved.y-y0;
        // The selection moves with it (when it is one of them), and a frame takes what it holds along.
        if (dx||dy) {
            const std::vector<int> movers=DragMovers();
            const auto inside=[&](int i){ for (const int f:movers) if (f!=i&&IsFrame(f)&&NpcCombo::InGroup(config_,i,f)) return true; return false; };
            for (size_t i=0;i<config_.chain.size();++i) {
                const int index=static_cast<int>(i);
                if (index==drag_.index) continue;
                const bool mover=std::find(movers.begin(),movers.end(),index)!=movers.end();
                if (!inside(index)&&!mover) continue;
                config_.chain[i].x=NpcCombo::ClampPosition(config_.chain[i].x+dx); config_.chain[i].y=NpcCombo::ClampPosition(config_.chain[i].y+dy);
            }
        }
    }
    else if (drag_.mode==Drag::Resize&&IsFrame(drag_.index)) {
        // Never smaller than what it holds.
        Node& frame=config_.chain[static_cast<size_t>(drag_.index)];
        int w=NpcCombo::ClampGroupW(frame.w),h=NpcCombo::ClampGroupH(frame.h);
        shift(w,h);
        const HDC dc=GetDC(window_); int need_w=0,need_h=0;
        for (size_t i=0;i<config_.chain.size();++i) if (config_.chain[i].group==drag_.index) {
            const SIZE s=NodeSize(dc,i);
            need_w=(std::max)(need_w,config_.chain[i].x+static_cast<int>(s.cx)+12-frame.x); need_h=(std::max)(need_h,config_.chain[i].y+static_cast<int>(s.cy)+12-frame.y);
        }
        ReleaseDC(window_,dc);
        frame.w=NpcCombo::ClampGroupW((std::max)(w,need_w)); frame.h=NpcCombo::ClampGroupH((std::max)(h,need_h));
    }
    else if (drag_.mode==Drag::Pan) { pan_x_+=static_cast<float>(p.x-drag_.last.x); pan_y_+=static_cast<float>(p.y-drag_.last.y); if (search_.open) SearchLayout(); }
    drag_.last=p;
    Invalidate();
}
void DragEnd(POINT p) {
    const DragState done=drag_; drag_={};
    if (GetCapture()==window_) ReleaseCapture();
    if (!done.moved) {
        if (done.right) { if (PtInRect(&canvas_,p)) OpenSearch(p); return; }  // right-click: search
        if (done.mode==Drag::Node&&At(done.index)) { picked_.clear(); Select(done.index); }  // a plain click: that node alone
        const Hit hit=HitTest(p);
        if (hit==done.hit) Click(hit);
        Invalidate(); return;
    }
    switch (done.mode) {
    case Drag::Node: {
        const HDC dc=GetDC(window_);
        drag_.index=done.index; const std::vector<int> movers=DragMovers(); drag_.index=-1;
        for (const int i:movers) Rejoin(dc,i);
        ReleaseDC(window_,dc); Save(); break;
    }
    case Drag::Box: {
        // Every card the box touches joins the selection (a 组合 frame only when the box holds all of it).
        drag_.start=done.start; drag_.last=p; const RECT box=BoxRect(); drag_={};
        const HDC dc=GetDC(window_);
        std::vector<int> picked=Selection();
        for (size_t i=0;i<config_.chain.size();++i) {
            const RECT card=NodeShape(dc,i).rect; RECT overlap{};
            const bool in=IsFrame(static_cast<int>(i))?card.left>=box.left&&card.top>=box.top&&card.right<=box.right&&card.bottom<=box.bottom:IntersectRect(&overlap,&card,&box)!=FALSE;
            if (in&&std::find(picked.begin(),picked.end(),static_cast<int>(i))==picked.end()) picked.push_back(static_cast<int>(i));
        }
        ReleaseDC(window_,dc);
        picked_=picked; selected_=picked_.empty()?-1:picked_.back();
        if (picked_.size()==1) picked_.clear();
        Invalidate(); break;
    }
    case Drag::Resize: Save(); break;
    case Drag::Arrow: case Drag::ArrowBack: {
        // An arrow from a node's round pin onto a modifier (or from a modifier's pin back onto a node): that node
        // points at it, where it may. From a node's pin onto empty canvas: the search adds the modifier.
        if (!At(done.index)) break;
        const int target=NodeAt(p);
        const int source=done.mode==Drag::Arrow?done.index:target,mod=done.mode==Drag::Arrow?target:done.index;
        if (target<0) { if (done.mode==Drag::Arrow&&PtInRect(&canvas_,p)) OpenModSearch(p,done.index); break; }
        if (!NpcCombo::IsModifier(config_.chain[static_cast<size_t>(mod)])) { DebugHub::Notify(Tr(L"箭头要拖到加速、霸体、允许中断或武器属性节点上。").c_str()); break; }
        if (NpcCombo::LinkModifier(config_,source,mod)) { Save(); break; }
        DebugHub::Notify(Tr(config_.chain[static_cast<size_t>(mod)].type==NodeType::Weapon?L"武器属性只能由起端节点（NPC 技能键或替换按键）指向。":L"只有起端节点和招式能指向加速、霸体、允许中断。").c_str());
        break;
    }
    case Drag::Wire: {
        // Onto a node: wired; onto empty canvas: unwired, and the search picks what to wire to (as in Blueprints).
        if (!At(done.index)) break;
        const int target=NodeAt(p);
        const size_t out=static_cast<size_t>((std::max)(0,done.sub));
        if (target>=0&&!SameFrame(done.index,target)) { DebugHub::Notify(Tr(L"组合里的节点只和同一个组合里的节点连线；组合本身从框头的引脚接入派生链。").c_str()); break; }
        if (target>=0) NpcCombo::Link(config_,done.index,target,out);
        else { NpcCombo::Link(config_,done.index,-1,out); if (PtInRect(&canvas_,p)) OpenSearch(p,done.index,-1,static_cast<int>(out)); }
        Save(); break;
    }
    case Drag::WireBack: {
        // From an exec input back to an output: a branch's row pin, else its first unwired row.
        const Hit hit=HitTest(p);
        const int source=hit.part>=Part::Node&&hit.part<=Part::ModIn?hit.index:-1;
        if (source>=0&&!SameFrame(source,done.index)) DebugHub::Notify(Tr(L"组合里的节点只和同一个组合里的节点连线；组合本身从框头的引脚接入派生链。").c_str());
        else if (source>=0&&!NpcCombo::IsModifier(config_.chain[static_cast<size_t>(source)])) {
            const Node& s=config_.chain[static_cast<size_t>(source)];
            size_t out=0;
            if (hit.part==Part::RouteOut) out=static_cast<size_t>(hit.sub);
            else if (NpcCombo::HasRows(s)) for (size_t r=0;r<s.routes.size();++r) if (s.routes[r].next<0) { out=r; break; }
            NpcCombo::Link(config_,source,done.index,out);
        }
        else if (source<0&&PtInRect(&canvas_,p)) OpenSearch(p,-1,done.index,0);
        Save(); break;
    }
    case Drag::WeaponRow:
        if (PtInRect(&canvas_,p)) DropWeapon(done.index,p);
        break;
    case Drag::Pill:
        if (PtInRect(&canvas_,p)&&static_cast<size_t>(done.index)<pills_.size()) {
            Node node; node.ability=pills_[static_cast<size_t>(done.index)].ability;
            Place(node,ToCanvasX(p.x)-30,ToCanvasY(p.y)-kHeader/2.f);
        }
        break;
    default: break;
    }
    Invalidate();
}
// Alt + click on a pin breaks its wires, as in the Blueprint editor.
void BreakPin(const Hit& hit) {
    if (hit.part==Part::NodeOut) NpcCombo::Link(config_,hit.index,-1);
    else if (hit.part==Part::RouteOut) NpcCombo::Link(config_,hit.index,-1,static_cast<size_t>((std::max)(0,hit.sub)));
    else if (hit.part==Part::NodeIn)
        for (Node& node:config_.chain) for (size_t o=0;o<NpcCombo::OutCount(node);++o) { if (NpcCombo::OutOf(node,o)==hit.index) NpcCombo::SetOut(node,o,-1); }
    else if (hit.part==Part::ModOut) NpcCombo::UnlinkModifiers(config_,hit.index);
    else if (hit.part==Part::ModIn) NpcCombo::UnlinkInto(config_,hit.index);
    Save(); Invalidate();
}
void ZoomAt(POINT p,int steps) {
    const int next=std::clamp(zoom_index_+steps,0,static_cast<int>(std::size(kZooms))-1);
    if (next==zoom_index_) return;
    const float cx=ToCanvasX(p.x),cy=ToCanvasY(p.y);
    zoom_index_=next;
    pan_x_=p.x-canvas_.left-cx*Scale(); pan_y_=p.y-canvas_.top-cy*Scale();
    if (search_.open) SearchLayout();
    Invalidate();
}
void PressAt(POINT p,bool alt,WPARAM keys=0) {
    SetFocus(window_);
    const Hit hit=HitTest(p);
    if (search_.open&&hit.part!=Part::Search) { CloseSearch(); return; }  // a click outside closes the search
    if (edit_.node>=0&&!(hit.part==Part::NodeValue&&hit.index==edit_.node)) CommitEdit();  // a click elsewhere keeps the typed value
    pressed_=hit;
    switch (hit.part) {
    case Part::Search: SearchPick(hit.index); return;
    case Part::Pill: BeginDrag(Drag::Pill,hit,p,hit.index); break;
    case Part::WeaponRow: BeginDrag(Drag::WeaponRow,hit,p,hit.index); break;
    case Part::NodeOut: if (alt) BreakPin(hit); else { BeginDrag(Drag::Wire,hit,p,hit.index); Select(hit.index); } break;
    case Part::RouteOut: if (alt) BreakPin(hit); else { BeginDrag(Drag::Wire,hit,p,hit.index); drag_.sub=hit.sub; Select(hit.index); } break;
    case Part::NodeIn: if (alt) BreakPin(hit); else { BeginDrag(Drag::WireBack,hit,p,hit.index); Select(hit.index); } break;
    case Part::ModOut: if (alt) BreakPin(hit); else { BeginDrag(Drag::Arrow,hit,p,hit.index); Select(hit.index); } break;
    case Part::ModIn: if (alt) BreakPin(hit); else { BeginDrag(Drag::ArrowBack,hit,p,hit.index); Select(hit.index); } break;
    case Part::Node: case Part::NodeValue: case Part::RouteValue:
        if (hit.part==Part::Node&&(keys&MK_CONTROL)) { TogglePick(hit.index); break; }  // Ctrl + click: in or out of the selection
        BeginDrag(Drag::Node,hit,p,hit.index); Select(hit.index); break;
    case Part::GroupResize: BeginDrag(Drag::Resize,hit,p,hit.index); Select(hit.index); break;
    case Part::Canvas: BeginDrag((keys&(MK_CONTROL|MK_SHIFT))?Drag::Box:Drag::Pan,hit,p,-1); break;  // Ctrl / Shift: 框选
    case Part::None: break;
    default: BeginDrag(Drag::Pending,hit,p,hit.index); break;  // buttons and node controls: click on release
    }
    Invalidate();
}
bool SearchKey(WPARAM key) {
    if (!search_.open) return false;
    switch (key) {
    case VK_ESCAPE: case VK_TAB: CloseSearch(); return true;
    case VK_RETURN: SearchPick(search_.selected); return true;
    case VK_UP: SearchMove(-1); return true;
    case VK_DOWN: SearchMove(1); return true;
    case VK_PRIOR: SearchMove(-SearchVisible()); return true;
    case VK_NEXT: SearchMove(SearchVisible()); return true;
    case VK_BACK: if (!search_.query.empty()) { search_.query.pop_back(); SearchFilter(); Invalidate(); } return true;
    default: return true;  // every other key belongs to the search box while it is open
    }
}
LRESULT CALLBACK Proc(HWND window,UINT message,WPARAM wparam,LPARAM lparam) {
    switch (message) {
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: Paint(); return 0;
    case WM_PRINTCLIENT: Paint(reinterpret_cast<HDC>(wparam)); return 0;
    case WM_SIZE: Layout(); return 0;
    case WM_DPICHANGED_AFTERPARENT: DropFonts(); Layout(); return 0;
    case WM_MOUSEMOVE: {
        const POINT p{GET_X_LPARAM(lparam),GET_Y_LPARAM(lparam)};
        if (drag_.mode!=Drag::None) { DragMove(p); return 0; }
        cursor_=p;
        const Hit hit=HitTest(p);
        if (!(hit==hot_)) { hot_=hit; Invalidate(); }
        if (search_.open) { const int row=SearchRowAt(p); if (row!=search_.hot) { search_.hot=row; Invalidate(); } }
        TRACKMOUSEEVENT track{sizeof(track),TME_LEAVE,window,0}; TrackMouseEvent(&track);
        return 0;
    }
    case WM_MOUSELEAVE: if (drag_.mode==Drag::None) { hot_={}; search_.hot=-1; Invalidate(); } return 0;
    case WM_SETCURSOR:
        if (LOWORD(lparam)==HTCLIENT) {
            const Part part=hot_.part;
            if (drag_.mode==Drag::Resize||part==Part::GroupResize) { SetCursor(LoadCursorW(nullptr,IDC_SIZENWSE)); return TRUE; }
            if (drag_.mode==Drag::Box) { SetCursor(LoadCursorW(nullptr,IDC_CROSS)); return TRUE; }
            if (drag_.mode==Drag::Pan||drag_.mode==Drag::Node) { SetCursor(LoadCursorW(nullptr,IDC_SIZEALL)); return TRUE; }
            if (part==Part::Node) { SetCursor(LoadCursorW(nullptr,IDC_SIZEALL)); return TRUE; }
            if (part!=Part::None&&part!=Part::Canvas) { SetCursor(LoadCursorW(nullptr,IDC_HAND)); return TRUE; }
        }
        break;
    case WM_LBUTTONDOWN:
        if (record_.node>=0) { StopRecording(L"录制结束"); return 0; }
        PressAt({GET_X_LPARAM(lparam),GET_Y_LPARAM(lparam)},GetKeyState(VK_MENU)<0,wparam); return 0;
    case WM_XBUTTONDOWN: case WM_XBUTTONUP:
        if (record_.node>=0) { if (message==WM_XBUTTONDOWN) RecordKey(GET_XBUTTON_WPARAM(wparam)==XBUTTON1?VK_XBUTTON1:VK_XBUTTON2); return TRUE; }
        break;
    case WM_LBUTTONUP: pressed_={}; if (drag_.mode!=Drag::None&&!drag_.right) DragEnd({GET_X_LPARAM(lparam),GET_Y_LPARAM(lparam)}); return 0;
    case WM_RBUTTONDOWN: case WM_MBUTTONDOWN: {
        if (record_.node>=0) { if (message==WM_MBUTTONDOWN) RecordKey(VK_MBUTTON); else StopRecording(L"录制结束"); return 0; }
        const POINT p{GET_X_LPARAM(lparam),GET_Y_LPARAM(lparam)};
        SetFocus(window);
        if (search_.open&&!PtInRect(&search_.rect,p)) CloseSearch();
        if (HasSkin()&&PtInRect(&canvas_,p)&&drag_.mode==Drag::None) BeginDrag(Drag::Pan,{},p,-1,message==WM_RBUTTONDOWN);
        return 0;
    }
    case WM_RBUTTONUP: case WM_MBUTTONUP: if (drag_.mode==Drag::Pan) DragEnd({GET_X_LPARAM(lparam),GET_Y_LPARAM(lparam)}); return 0;
    case WM_CAPTURECHANGED: if (drag_.mode!=Drag::None&&reinterpret_cast<HWND>(lparam)!=window) { drag_={}; Invalidate(); } return 0;
    case WM_MOUSEWHEEL: {
        POINT p{GET_X_LPARAM(lparam),GET_Y_LPARAM(lparam)}; ScreenToClient(window,&p);
        const int steps=GET_WHEEL_DELTA_WPARAM(wparam)>0?1:-1;
        if (search_.open&&PtInRect(&search_.rect,p)) {
            search_.scroll=std::clamp(search_.scroll-steps*3,0,(std::max)(0,static_cast<int>(search_.rows.size())-SearchVisible())); Invalidate(); return 0;
        }
        if (PtInRect(&strip_,p)&&strip_left_.right>strip_left_.left) { ScrollStrip(steps>0?-M().S(160):M().S(160)); return 0; }
        if (HasSkin()&&PtInRect(&weapons_,p)) { weapon_scroll_-=steps*M().S(90); Layout(); Invalidate(); return 0; }
        if (!HasSkin()||!PtInRect(&canvas_,p)) break;  // the hub scrolls the page
        const Hit hit=HitTest(p);
        CommitEdit();
        if (hit.part==Part::NodeValue||hit.part==Part::NodeMinus||hit.part==Part::NodePlus) Nudge(hit.index,steps);
        else if (hit.part==Part::RouteValue||hit.part==Part::RouteMinus||hit.part==Part::RoutePlus) Nudge(hit.index,steps,hit.sub);
        else ZoomAt(p,steps);
        return 0;
    }
    case WM_GETDLGCODE: return DLGC_WANTARROWS|DLGC_WANTTAB|DLGC_WANTCHARS|(search_.open||edit_.node>=0||record_.node>=0?DLGC_WANTALLKEYS:0);
    case WM_SYSKEYDOWN:
        if (record_.node>=0) { if (!(lparam&(1<<30))) RecordKey(static_cast<int>(wparam)); return 0; }
        break;
    case WM_CHAR:
        if (edit_.node>=0) { EditChar(static_cast<wchar_t>(wparam)); return 0; }
        if (search_.open&&wparam>=32&&wparam!=127) { search_.query.push_back(static_cast<wchar_t>(wparam)); SearchFilter(); Invalidate(); }
        return 0;
    case WM_KEYDOWN:
        if (record_.node>=0) {
            if (wparam==VK_ESCAPE) StopRecording(L"录制结束");
            else if (!(lparam&(1<<30))) RecordKey(static_cast<int>(wparam));  // not a held key's repeat
            return 0;
        }
        if (EditKey(wparam)) return 0;
        if (SearchKey(wparam)) return 0;
        if (wparam==VK_TAB) { POINT p=cursor_; if (!PtInRect(&canvas_,p)) p={(canvas_.left+canvas_.right)/2,(canvas_.top+canvas_.bottom)/3}; OpenSearch(p); return 0; }
        if (wparam==VK_DELETE||wparam==VK_BACK) { RemoveSelected(); return 0; }
        if (wparam==VK_HOME) { Frame(); return 0; }
        if ((wparam==VK_UP||wparam==VK_DOWN)&&At(selected_)) { Nudge(selected_,wparam==VK_UP?1:-1); return 0; }
        break;
    case WM_COMMAND: if (LOWORD(wparam)==IDCANCEL) { if (record_.node>=0) { StopRecording(L"录制结束"); return 0; } if (edit_.node>=0) { CancelEdit(); return 0; } if (search_.open) { CloseSearch(); return 0; } DebugHub::Hide(); return 0; } break;
    case WM_CLOSE: DebugHub::Hide(); return 0;
    case WM_DESTROY: window_=nullptr; return 0;
    default: break;
    }
    return DefWindowProcW(window,message,wparam,lparam);
}
}

HWND CreatePage(HWND parent) {
    if (!parent) return nullptr;
    if (!window_) {
        WNDCLASSEXW cls{}; cls.cbSize=sizeof(cls); cls.hInstance=GetModuleHandleW(nullptr); cls.hCursor=LoadCursorW(nullptr,IDC_ARROW);
        cls.lpszClassName=kClass; cls.lpfnWndProc=&Proc;
        if (!RegisterClassExW(&cls)&&GetLastError()!=ERROR_CLASS_ALREADY_EXISTS) return nullptr;
        window_=CreateWindowExW(0,kClass,L"招式派生",WS_CHILD|WS_TABSTOP,0,0,800,650,parent,nullptr,GetModuleHandleW(nullptr),nullptr);
        if (!window_) return nullptr;
        language_=UiI18n::GetLanguage();
        DebugHub::Attach(window_,parent,460,600);
    }
    Refresh(true); return window_;
}
void Tick() {
    if (!window_||!IsWindowVisible(window_)) { if (record_.node>=0) record_={}; return; }
    if (record_.node>=0) {  // a pad press joins the recorded keys (every worker tick)
        const UiHotkeys::PadLevel level=UiHotkeys::CurrentPad();
        const unsigned pad=NpcCombo::PadCodeBits(level.buttons,level.stick);  // buttons, then the left stick's way (上推 / 下推 ...)
        const unsigned went=record_.pad_seeded?pad&~record_.pad_was:0;
        record_.pad_was=pad; record_.pad_seeded=true;
        for (int bit=0;bit<NpcCombo::kPadBits+NpcCombo::kStickWays&&record_.node>=0;++bit) if (went&(1u<<bit)) RecordKey(NpcCombo::kPadKey+bit);
    }
    const ULONGLONG now=GetTickCount64();
    if (now<next_) return;
    next_=now+150;
    if (UiI18n::GetLanguage()!=language_) { language_=UiI18n::GetLanguage(); Layout(); }
    if (const TestDummy::Snapshot dummy=TestDummy::GetSnapshot(); dummy.present!=dummy_present_||dummy.status!=dummy_status_) {
        if (dummy.status!=dummy_status_&&!dummy.status.empty()) DebugHub::Notify(Tr(dummy.status.c_str()).c_str());  // what the game thread did
        dummy_status_=dummy.status;
        if (dummy.present!=dummy_present_) { dummy_present_=dummy.present; Layout(); Invalidate(); }
    }
    if (drag_.mode==Drag::None&&!search_.open&&edit_.node<0) Refresh(false);
    if (search_.open||edit_.node>=0) Invalidate();  // the caret blinks
}
namespace Probe {
int Key() { return NpcCombo::KeyTrigger(config_); }
POINT Header(int node) {
    if (node<0) node=Key();
    const HDC dc=GetDC(window_);
    const Shape s=NodeShape(dc,static_cast<size_t>(node));
    ReleaseDC(window_,dc);
    return {(s.header.left+s.header.right)/2-Px(20),(s.header.top+s.header.bottom)/2};
}
POINT Pin(int node,bool out) {
    if (node<0) node=Key();
    const HDC dc=GetDC(window_);
    const Shape s=NodeShape(dc,static_cast<size_t>(node));
    ReleaseDC(window_,dc);
    return out?s.out:s.in;
}
POINT Field(int node) {
    const HDC dc=GetDC(window_);
    const Shape s=NodeShape(dc,static_cast<size_t>(node));
    ReleaseDC(window_,dc);
    return {(s.field.left+s.field.right)/2,(s.field.top+s.field.bottom)/2};
}
Shape ShapeOf(int node) { const HDC dc=GetDC(window_); const Shape s=NodeShape(dc,static_cast<size_t>(node)); ReleaseDC(window_,dc); return s; }
POINT Middle(const RECT& r) { return {(r.left+r.right)/2,(r.top+r.bottom)/2}; }
POINT RouteOut(int node,int route) { return ShapeOf(node).routes.at(static_cast<size_t>(route)).out; }
POINT RouteField(int node,int route) { return Middle(ShapeOf(node).routes.at(static_cast<size_t>(route)).field); }
POINT RoutePlus(int node,int route) { return Middle(ShapeOf(node).routes.at(static_cast<size_t>(route)).plus); }
POINT RouteKeys(int node,int route) { const RouteShape r=ShapeOf(node).routes.at(static_cast<size_t>(route)); return r.has_keys?Middle(r.keys):POINT{-1,-1}; }
POINT RouteMode(int node,int route) { const RouteShape r=ShapeOf(node).routes.at(static_cast<size_t>(route)); return r.has_keys?Middle(r.mode):POINT{-1,-1}; }
POINT RouteAdd(int node) { return Middle(ShapeOf(node).add); }
POINT ModOut(int node) { return ShapeOf(node).mod_out; }
POINT ModIn(int node) { return ShapeOf(node).mod_in; }
POINT Value(int node) { return Middle(ShapeOf(node).value); }
bool Editing() { return edit_.node>=0; }
POINT Plus(int node) { return Middle(ShapeOf(node).plus); }
POINT Minus(int node) { return Middle(ShapeOf(node).minus); }
POINT ArrowHead(int node,int slot) {
    const int mod=NpcCombo::ModOf(config_,node,slot); if (mod<0) return {-1,-1};
    const Shape from=ShapeOf(node),target=ShapeOf(mod);
    return ArrowHeadPoint(from.mod_out,ArrowTip(from.mod_out,target.rect,target.mod_in));
}
COLORREF ArrowColor(int node,int slot) { const int mod=NpcCombo::ModOf(config_,node,slot); return mod<0?0:ModColor(config_.chain[static_cast<size_t>(mod)].type); }
RECT NodeRect(int node) { return ShapeOf(node).rect; }
void MoveTo(int node,POINT p) {
    if (!At(node)) return;
    config_.chain[static_cast<size_t>(node)].x=static_cast<int>(std::lround(ToCanvasX(p.x)))-20;
    config_.chain[static_cast<size_t>(node)].y=static_cast<int>(std::lround(ToCanvasY(p.y)))-14;
    Invalidate();
}
int FreeTop() { return canvas_.top+static_cast<int>(std::lround(ToolsBottom())); }
void CenterOn(POINT p) {
    // The middle of the canvas under the floating tool bar (a narrow page wraps it over the canvas's top half).
    const float top=static_cast<float>(canvas_.top)+ToolsBottom();
    pan_x_+=static_cast<float>((canvas_.left+canvas_.right)/2-p.x); pan_y_+=(top+static_cast<float>(canvas_.bottom))/2.f-static_cast<float>(p.y);
    Invalidate();
}
void ZoomTo(int percent) { for (int i=0;i<static_cast<int>(std::size(kZooms));++i) if (std::lround(kZooms[i]*100)==percent) { zoom_index_=i; Invalidate(); } }
POINT Pill(int index) { const RECT r=pills_.at(static_cast<size_t>(index)).rect; return {(r.left+r.right)/2,(r.top+r.bottom)/2}; }
RECT Canvas() { return canvas_; }
POINT Tool(int button) { const RECT r=buttons_[button]; return {(r.left+r.right)/2,(r.top+r.bottom)/2}; }
int Zoom() { return static_cast<int>(std::lround(kZooms[zoom_index_]*100)); }
bool SearchOpen() { return search_.open; }
size_t Picked() { return Selection().size(); }
bool PickedNode(int node) { const std::vector<int> s=Selection(); return std::find(s.begin(),s.end(),node)!=s.end(); }
bool Empty(POINT p) { return HitTest(p).part==Part::Canvas; }
// Next to the other node on a clear spot: under it, else to its right, else under its right; failing those, under
// it stepped further down past any node it would cover.
void Put(int node,int below) {
    if (!At(node)||!At(below)||node==below) return;
    const HDC dc=GetDC(window_);
    const SIZE size=NodeSize(dc,static_cast<size_t>(node)),bs=NodeSize(dc,static_cast<size_t>(below));
    Node& n=config_.chain[static_cast<size_t>(node)]; const Node b=config_.chain[static_cast<size_t>(below)];
    const auto covers=[&](int x,int y) -> int {
        for (size_t i=0;i<config_.chain.size();++i) {
            if (static_cast<int>(i)==node) continue;
            const Node& o=config_.chain[i]; const SIZE s=NodeSize(dc,i);
            if (x<o.x+s.cx+10&&o.x<x+size.cx+10&&y<o.y+s.cy+10&&o.y<y+size.cy+10) return static_cast<int>(i);
        }
        return -1;
    };
    const POINT spots[]={{b.x,b.y+bs.cy+30},{b.x+bs.cx+60,b.y},{b.x+bs.cx+60,b.y+bs.cy+30}};
    bool found=false;
    for (const POINT& p:spots) if (covers(p.x,p.y)<0) { n.x=p.x; n.y=p.y; found=true; break; }
    if (!found) {
        n.x=spots[0].x; n.y=spots[0].y;
        for (int o=covers(n.x,n.y);o>=0;o=covers(n.x,n.y)) n.y=config_.chain[static_cast<size_t>(o)].y+static_cast<int>(NodeSize(dc,static_cast<size_t>(o)).cy)+30;
    }
    ReleaseDC(window_,dc); Save(); Invalidate();
}
void Show(int node) { if (!At(node)) return; const Node& n=config_.chain[static_cast<size_t>(node)]; pan_x_=M().S(60)-n.x*Scale(); pan_y_=ToolsBottom()+M().S(20)-n.y*Scale(); Invalidate(); }
size_t SearchRows() { return search_.rows.size(); }
POINT WeaponRowOf(const char* id) {
    const int top=weapons_title_.bottom+M().S(4);
    for (const WeaponRow& row:weapon_rows_)
        if (!row.header&&row.id==id) return row.rect.top>=top&&row.rect.bottom<=weapons_.bottom?Middle(row.rect):POINT{-1,-1};
    return {-1,-1};
}
size_t WeaponRows() { return static_cast<size_t>(std::count_if(weapon_rows_.begin(),weapon_rows_.end(),[](const WeaponRow& r){return !r.header;})); }
POINT Field2(int node) { return Middle(ShapeOf(node).field2); }
POINT Field3(int node) { return Middle(ShapeOf(node).field3); }
POINT Field4(int node) { return Middle(ShapeOf(node).field4); }
POINT Header() { return Middle(header_); }
std::string EditedProfile() { return moves_.profile; }
POINT Toggle2(int node) { return Middle(ShapeOf(node).toggle2); }
int RecordingNode() { return record_.node; }
int RecordingRoute() { return record_.node>=0?record_.route:-1; }
void ScrollWeapons(int pixels) { weapon_scroll_+=pixels; Layout(); Invalidate(); }
POINT Resize(int node) { return Middle(ShapeOf(node).resize); }
// An empty point in a frame's body (left, a little under the middle).
POINT Body(int node) { const RECT r=ShapeOf(node).rect; return {r.left+(r.right-r.left)/6,r.top+(r.bottom-r.top)*11/20}; }
}
void Shutdown() {
    if (window_) DestroyWindow(window_);
    window_=nullptr; pills_.clear(); tabs_.clear(); skin_=-1; pinned_=kFollowGame; game_chain_=-1; moves_={}; config_={}; search_={}; DropFonts();
}
}
