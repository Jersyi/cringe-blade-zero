#include "ui_paint_buffer.h"
#include "combo_menu.h"
#include "npc_combo.h"
#include "npc_retarget.h"
#include "debug_hub.h"
#include "motian_grab.h"
#include "test_dummy.h"
#include "move_preview.h"
#include "speed_curve_window.h"
#include "bone_motion_page.h"
#include "grab_boss.h"
#include "combo_weapons.h"
#include "weapon_icons.h"
#include "ui_cursor.h"
#include "ui_host.h"
#include "ui_hotkeys.h"
#include "ui_i18n.h"
#include "ui_layout.h"
#include "ui_theme.h"
#include "ui_motion.h"
#include "ui_surface.h"
#include "npc_boss_effects.h"
#include <objidl.h>
#include <shobjidl.h>
#include <gdiplus.h>
#include <windowsx.h>
#include <commctrl.h>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdarg>
#include <cwchar>
#include <cwctype>
#include <map>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "shell32.lib")

// The page is laid out like a Blueprint graph, in the hub's own colours and fonts: the skin's moves by kind on top
// (click to append, drag onto the canvas to place), then a canvas of nodes with exec pins and wires. Trigger nodes
// (the NPC skill key, or one of the hero's own buttons it replaces) start chains; every press plays the next move,
// a 按键分支 picks the next one by the button pressed (and the pause before it). 加速 / 霸体 / 武器属性 nodes point
// with a round pin at a trigger (its whole chain) or, 加速 / 霸体, at one move. Nodes are dragged freely, wires drawn from
// pin to pin, the canvas pans and zooms, Tab / right-click searches.
namespace ComboMenu {
namespace {
void (*log_)(const char*)=nullptr;
void Logf(const char* format,...) {
    if (!log_) return;
    char line[700]{}; va_list args; va_start(args,format); _vsnprintf_s(line,sizeof(line),_TRUNCATE,format,args); va_end(args); log_(line);
}
volatile LONG test_curve_=0;  // combo curve: 1 open, 2 close (UI worker)
using NpcCombo::Kind;
using NpcCombo::Node;
using NpcCombo::NodeType;
using NpcCombo::Trigger;
constexpr wchar_t kClass[]=L"PBZComboPage_v3";
// Canvas zoom steps (the wheel moves one): far enough out that 显示全部 fits a chain of any size.
constexpr float kZooms[]={.05f,.075f,.1f,.15f,.2f,.25f,.3f,.4f,.5f,.625f,.75f,.875f,1.f,1.25f,1.5f};
constexpr int ZoomIndex(float zoom) { for (int i=0;i<static_cast<int>(std::size(kZooms));++i) if (kZooms[i]==zoom) return i; return 0; }
constexpr int kZoomOne=ZoomIndex(1.f),kZoomOpen=ZoomIndex(.75f);  // 1:1; the opening view's smallest
// Under this zoom a card is drawn as its body and title only (the overview), and the whole card is one target.
constexpr float kOverviewZoom=.35f;
// Node geometry in canvas units (96 dpi, zoom 1).
constexpr int kHeader=28,kRow=24,kPad=8,kPin=6,kGrid=16,kMajor=128;
enum class Button : int { Toggle, Interval, AddTrigger, Arrange, Frame, Remove, Clear, Branch, Speed, Weapon, Armor, Interrupt, Grab, BossSkill, File, SaveFile, SaveAs, WeaponMove, Damage,
                          Group, Teleport, Invincible, Look, HeroLook, KeepLook, ForceHit, WeaponActions, Toast, Dummy, AutoDodge, IgnoreGuard, KeepChain, Assist, Hook, Trim, Air, AirSet, BoneTransform,
                          Copy, Paste, Hud, SkillDamage, GrabDamage, BossEffect, ViewTarget, Count };  // Hud: 派生显示 (关 / 详细 / 简略), beside 派生提示;
                                                                                // ViewTarget: 镜头跟随 (骨盆 / 脖子 / 头部), beside 派生显示;
                                                                                // SkillDamage / GrabDamage: 派生页的技能伤害 / 投技伤害
// The canvas tools, laid out right to left.
constexpr Button kTools[]={Button::Copy,Button::Paste,Button::SaveAs,Button::SaveFile,Button::File,Button::Clear,Button::Remove,Button::Frame,Button::Arrange,Button::Weapon,Button::KeepChain,Button::Interrupt,Button::IgnoreGuard,Button::Invincible,Button::BossEffect,Button::Air,Button::ForceHit,Button::Armor,Button::Damage,Button::BoneTransform,Button::Speed,Button::Trim,Button::AddTrigger,Button::Branch,Button::Grab,Button::AirSet,Button::WeaponActions,Button::WeaponMove,Button::Hook,Button::Teleport,Button::HeroLook,Button::KeepLook,Button::Look,Button::Assist,Button::BossSkill,Button::Group,Button::AutoDodge,Button::Interval};
// Management stays above the canvas; creation tools belong to the right library.
constexpr Button kCanvasTools[]={Button::File,Button::SaveFile,Button::SaveAs,Button::Arrange,Button::Frame,Button::Copy,Button::Paste,Button::Remove,Button::Clear};
constexpr const wchar_t* kNodeCategories[]={L"起端与流程",L"动作与移动",L"战斗修饰",L"外观变身",L"BOSS 招式"};
constexpr int NodeCategory(Button b) {
    switch (b) {
    case Button::AddTrigger: case Button::Branch: case Button::AirSet: case Button::WeaponActions: case Button::Interval: case Button::Group: return 0;
    case Button::BossSkill: case Button::Assist: case Button::Grab: case Button::WeaponMove: case Button::AutoDodge: case Button::Teleport: case Button::Hook: return 1;
    case Button::Speed: case Button::Trim: case Button::Weapon: case Button::Armor: case Button::Interrupt: case Button::Damage:
    case Button::Invincible: case Button::ForceHit: case Button::IgnoreGuard: case Button::KeepChain: case Button::Air: case Button::BossEffect: return 2;
    case Button::BoneTransform: case Button::Look: case Button::HeroLook: case Button::KeepLook: return 3;
    default: return -1;
    }
}
constexpr bool LibraryComplete() {
    for (Button b:kTools) {
        int owners=NodeCategory(b)>=0?1:0;
        for (Button action:kCanvasTools) if (action==b) ++owners;
        if (owners!=1) return false;
    }
    return true;
}
static_assert(LibraryComplete(),"each tool belongs to exactly one library category or canvas action");

enum class Part : int {
    None, Button, Pill, Tab, StripLeft, StripRight, Canvas, Search, WeaponRow, LibraryTab, NodeCategory,
    Node, NodeIn, NodeOut, NodeMinus, NodePlus, NodeValue, NodeField, NodeField2, NodeField3, NodeField4, NodeToggle, NodeToggle2, NodeHoldMinus, NodeHoldPlus, NodeHoldValue, NodeTrimBar,
    GroupResize, NodeRemove,
    RouteField, RouteMinus, RoutePlus, RouteValue, RouteHoldMinus, RouteHoldPlus, RouteHoldValue, RouteRemove, RouteAdd, RouteOut, ModOut, ModIn, RouteKeys, RouteMode, Header,
    NodeToggle3,  // a chain start's 整体不变身
    LibraryBoss   // the BOSS 招式 list's 招式来源 dropdown
};
// sub: a branch's route row (-1 otherwise).
struct Hit { Part part=Part::None; int index=-1; int sub=-1; bool operator==(const Hit& o) const { return part==o.part&&index==o.index&&sub==o.sub; } };
struct Pill { RECT rect{}; std::string ability; Kind kind=Kind::Other; };
struct Tab { int kind=-1; RECT rect{}; size_t count=0; };  // kind -1: every move
// A node's parts on screen. in: the exec input (a trigger: where its round pin sits); mod_out: a trigger's or a
// move's round pin, where its arrows to 加速 / 霸体 / 武器属性 start; mod_in: a modifier's round pin, where they end;
// routes: a branch's rows, each with its own exec output.
// A 自定义组合键 / 自定义按键 row has a second line: its keys and how they are pressed (has_keys).
// hminus / hvalue / hplus: a branch row's 按住 stepper (left of its pause).
struct RouteShape { RECT field{},minus{},value{},plus{},remove{},keys{},mode{},hminus{},hvalue{},hplus{}; POINT out{}; bool has_keys=false; };
struct Shape {
    RECT rect{},header{},remove{},minus{},value{},plus{},field{},field2{},field3{},field4{},toggle{},toggle2{},add{},resize{}; POINT in{},out{},mod_out{},mod_in{};
    RECT toggle3{};  // a chain start's 整体不变身 (its last row)
    RECT hminus{},hvalue{},hplus{};  // a trigger's 按住 stepper (has_hold)
    bool has_in=false,has_out=false,has_mod_out=false,has_mod_in=false,has_hold=false; std::vector<RouteShape> routes;
};
// Arrow: from a node's round pin to a modifier; ArrowBack: from a modifier's pin to the node that points at it.
enum class Drag { None, Pending, Node, Pan, Wire, WireBack, Arrow, ArrowBack, Pill, WeaponRow, Resize, Box, Trim };
struct DragState { Drag mode=Drag::None; Hit hit; POINT start{},last{}; bool moved=false,right=false; int index=-1,sub=-1; RECT trim_bar{}; int trim_clip=0; };
// The search box (Tab, right-click, or a wire dropped on empty canvas): pick a node to add where it opened. In pick
// mode (a trigger's or a route's button field) it lists the buttons and sets that field; on a 投技 node's field it
// lists the grabs of the look's boss (GrabMove).
// A BOSS 技能 node's fields list the looks (BossLook) and that look's moves (BossMove); a 变身 node's field lists every
// look (BossLook too); the 文件 button lists the
// .combo files (File). 另存为 (and 保存 when the active file cannot be written) opens Windows' own Save As window.
// A 武器派生 node's fields list the weapons (WeaponPick) and that weapon's abilities (WeaponSkill); a 处决 trigger's
// weapon field lists the weapons too, 任意武器 first.
enum class ItemKind { Move, Interval, AutoDodge, Trigger, Branch, Speed, Weapon, Armor, Interrupt, Grab, Button, GrabMove, BossSkill, BossLook, BossMove, File,
                      WeaponMove, Damage, WeaponPick, WeaponSkill, Group, Teleport, TeleportDist, TeleportLift, ComboKey, ComboMode, RawKey, Invincible,
                      Look, HeroLook, KeepLook, ForceHit, HitKind, WeaponActions, ThenKey, Chain, IgnoreGuard, KeepChain, Assist, Hook, HookMode, Trim,
                      HookDist, HookSpeed, Air, AirLift, AirHang, Launch, AirSet, BoneTransform, ChainSkill, ChainGrab,
                      BossEffect, FxFolder, FxPick, FxPlace, FxDamage, FxRadius, FxHits, MoveSource };
// value: a ChainSkill's multiplier (0: follow the NPC 技能 page), a ChainGrab's (trigger: its mode, kGrabFollow: follow the 投技 page).
struct Item { ItemKind kind=ItemKind::Move; std::wstring label,group,keys; std::string ability; int trigger=0; Kind move=Kind::Other; float value=0; };
struct SearchRow { bool header=false; size_t item=0; std::wstring text; };
struct Search {
    bool open=false; RECT rect{},edit{},list{}; float x=0,y=0; int from=-1,into=-1,from_out=0,pick_node=-1,pick_route=-1,mod_from=-1,mode=0; // mode 1 files, 2 save-as, 3 派生表, 4 技能伤害, 5 投技伤害
    int pick_part=0;  // a branch row's field (pick_route >= 0): 0 its button, 1 its keys, 2 how they are pressed
    // mode 6: the BOSS 招式 list's 招式来源 (this table's look or another boss)
    std::wstring query; std::vector<Item> items; std::vector<SearchRow> rows; int selected=-1,scroll=0,hot=-1;
    bool keyed=false;  // the selection was last moved with the keys (招式预览 follows it while the mouse is off the list)
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
int category_=-1;
RECT library_{},library_title_{},library_tabs_[2]{},node_categories_[5]{},library_list_{},library_page_label_{},library_section_{},library_category_title_{};
int library_tab_=0,node_category_=0,library_pages_[6]{},library_page_count_=1,library_capacity_=1,library_items_=0;
int LibrarySection() { return library_tab_==1?5:node_category_; }
// The BOSS 招式 list's source, the 招式来源 dropdown above its tabs (user 11-12): "" this table's own look; another look's
// id lists that boss's moves, and one of them clicked or dragged in becomes a BOSS 技能 of that boss, so the hero's table
// or any boss's can take other bosses' skills straight from the list. Kept while other tables are shown.
std::string library_boss_;RECT library_boss_box_{};
int height_=0,selected_=-1,zoom_index_=kZoomOne;
std::vector<Node> node_clipboard_;int paste_count_=0;
// Several nodes selected at once (Ctrl / Shift + drag on bare canvas: 框选; Ctrl + click a node adds or drops it):
// selected_ is one of them. Empty: selected_ alone. They are dragged together and removed together.
std::vector<int> picked_;
float pan_x_=0,pan_y_=0;  // canvas origin offset in screen pixels
bool framed_=false;
bool dummy_present_=false;  // 测试木桩 stands (TestDummy snapshot, read every Tick)
std::wstring dummy_status_;  // its last status, shown once as a notice
Hit hot_,pressed_;
// 招式预览: the move the mouse rests on (its look row and short GA name) and where it is on the page; after
// kPreviewDelayMs the game thread plays it on its stage (MovePreview) and its frames show in a card by it.
constexpr ULONGLONG kPreviewDelayMs=400;
// Tests (HandleCommand "preview"): a move held as if the mouse rested on it.
// node: as if the mouse rested on that canvas node ("combo preview node <n>"; -1: the skin's move).
struct TestHover { bool on=false; int32_t skin=-1; std::string ability; int node=-1; } test_hover_;
// Tests (combo scratch / combo zoom, from the console thread; the page applies them on its own thread): a chain shown
// in place of the look's, in memory only (never saved, the chain file untouched), and the canvas zoom.
SRWLOCK scratch_lock_=SRWLOCK_INIT; std::string scratch_ask_; bool scratch_asked_=false; int zoom_ask_=0;
bool scratch_=false; ULONGLONG paint_logged_=0;
// beside: the move is a row of the search box; the card goes by the box's side, not over its list.
// node: the canvas node the move is (its 加速 plays in the preview; -1: a pill or a search row, at ×1); speed / curve:
// what the preview was last asked to play at.
struct PreviewHover { int32_t skin=-1; std::string ability; RECT anchor{}; ULONGLONG since=0; bool asked=false,none=false,beside=false;
                      int node=-1; int speed=100; std::vector<NpcCombo::CurvePoint> curve; std::string monster,package;
                      int trim_from=0,trim_to=0; std::vector<int> trim_holds; BoneMotion::Config bones; };  // the 裁剪 part (ms of the montage; 0 / 0: all of it)
PreviewHover preview_; MovePreview::Frame preview_frame_; RECT preview_rect_{};
// The card's picture (page client) as last drawn; the hub's hole over it while the game draws the stage there (live).
RECT preview_image_{},preview_hole_rect_{}; bool preview_hole_=false;
DragState drag_;
Search search_;
HWND search_input_=nullptr;bool syncing_search_=false;
constexpr int kSearchInput=0x7b01;
bool SearchKey(WPARAM key);
void SyncSearchInput(){if(!search_input_)return;syncing_search_=true;
    if(UiLayout::Caption(search_input_)!=search_.query)SetWindowTextW(search_input_,search_.query.c_str());
    syncing_search_=false;}
void FocusSearch(){if(search_input_&&!UiCursor::HandsOff()){SetFocus(search_input_);SendMessageW(search_input_,EM_SETSEL,static_cast<WPARAM>(-1),-1);}}
LRESULT CALLBACK SearchInputProc(HWND w,UINT msg,WPARAM wp,LPARAM lp,UINT_PTR,DWORD_PTR){
    if(msg==WM_GETDLGCODE)return DLGC_WANTALLKEYS|DLGC_WANTCHARS;
    if(msg==WM_KEYDOWN){
        if(wp==VK_ESCAPE||wp==VK_RETURN||wp==VK_TAB||wp==VK_UP||wp==VK_DOWN||wp==VK_PRIOR||wp==VK_NEXT){SearchKey(wp);return 0;}
        if(wp=='A'&&(GetKeyState(VK_CONTROL)&0x8000)){SendMessageW(w,EM_SETSEL,0,-1);return 0;}}
    if(msg==WM_CHAR&&(wp==VK_RETURN||wp==VK_ESCAPE||wp==VK_TAB))return 0;
    return DefSubclassProc(w,msg,wp,lp);
}

// The 加速 multiplier being typed (node -1: none); fresh: the first key typed replaces the text.
// hold: what is typed: -2 the node's own value, -1 a trigger's 按住 (seconds), else that branch row's 按住.
struct Edit { int node=-1; std::wstring text; bool fresh=true; int hold=-2; } edit_;
POINT cursor_{-1,-1};
std::wstring hint_text_;
UiI18n::Language language_=UiI18n::Language::SimplifiedChinese;
ULONGLONG next_=0;
uint64_t run_serial_=0,weapon_serial_=0;
UINT font_dpi_=0;
std::map<int,HFONT> scaled_;
// Every paint and hit test asks for every card's size (text layout) and most notes for its chain (a walk of every
// trigger's wires): both are kept until the next change (Save, Invalidate and Layout drop them; another node count
// measures again), so a chain of thousands of nodes stays quick.
std::vector<SIZE> sizes_;
std::vector<int> owners_;
struct CanvasCache {
    std::vector<Shape> shapes;std::vector<size_t> order;
    std::vector<bool> live,fed,modded,pointed;bool graph_valid=false;
    float pan_x=0,pan_y=0,scale=0;RECT canvas{};
    uint64_t measurements=0,graph_builds=0,shape_builds=0;
    void Geometry(){shapes.clear();}
    void Clear(){Geometry();order.clear();graph_valid=false;}
} canvas_cache_;
void DropCanvasCache(){sizes_.clear();owners_.clear();canvas_cache_.Clear();}
void Repaint(){if(window_)InvalidateRect(window_,nullptr,FALSE);}


std::wstring Tr(const wchar_t* text) { return UiI18n::Translate(text?text:L""); }
UiLayout::Metrics M() { return UiLayout::For(window_); }
HFONT Font(UiTheme::Font role) { return UiTheme::GetFont(role,window_); }
float Zoom() { return kZooms[zoom_index_]; }
float Dpi() { return M().S(1000)/1000.f; }
float Scale() { return Zoom()*Dpi(); }
bool Overview() { return Zoom()<kOverviewZoom; }
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
    if(font)scaled_[key]=font;
    return font?font:Font(role);
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
// Text widths in canvas units, kept by font and text: a chain repeats its move names and notes, and measuring every
// card is the cost of each re-layout (dropped when the DPI changes and on Layout: language, fonts).
std::unordered_map<std::wstring,int> widths_; UINT widths_dpi_=0;
int Units(HDC dc,UiTheme::Font role,const std::wstring& text) {
    const UINT dpi=static_cast<UINT>(M().S(96));
    if (dpi!=widths_dpi_) { widths_.clear(); widths_dpi_=dpi; }
    std::wstring key; key.reserve(text.size()+1); key.push_back(static_cast<wchar_t>(L'A'+static_cast<int>(role))); key+=text;
    if (const auto found=widths_.find(key); found!=widths_.end()) return found->second;
    const int units=static_cast<int>(std::ceil(Extent(dc,Font(role),text).cx/Dpi()));
    if (widths_.size()>50000) widths_.clear();
    widths_.emplace(std::move(key),units);
    return units;
}
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
void Save() { DropCanvasCache(); if (scratch_) return; if (HasSkin()) { NpcCombo::Save(moves_.profile,config_); seen_generation_=NpcCombo::Generation(); } }
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
// The other look the BOSS 招式 list shows ("" when it shows this table's own moves).
std::string LibraryBoss() { return library_boss_.empty()||library_boss_==moves_.profile||BossLookRow(library_boss_)<0?std::string():library_boss_; }
// The node a move of the BOSS 招式 list adds: this table's move (当前外观招式), or another boss's as its BOSS 技能.
Node LibraryNode(const std::string& ability) {
    Node node; node.ability=ability;
    const std::string boss=LibraryBoss(); if (!boss.empty()) { node.type=NodeType::BossSkill; node.boss=boss; }
    return node;
}
COLORREF BossColor() { return UiTheme::HighContrast()?UiTheme::AccentBlue():UiTheme::Mix(RGB(206,92,64),UiTheme::Text(),12); }
// BOSS 援护: a boss of its own beside the hero.
COLORREF AssistColor() { return UiTheme::HighContrast()?UiTheme::AccentBlue():UiTheme::Mix(RGB(214,158,52),UiTheme::Text(),12); }
COLORREF WeaponColor() { return UiTheme::HighContrast()?UiTheme::AccentBlue():UiTheme::Mix(RGB(86,150,205),UiTheme::Text(),14); }
// The weapon list on the canvas's right: one row per weapon (main weapons, then 杀器 under a header), scrolled with
// the wheel; a row is clicked (a 武器派生 node after the selected one) or dragged onto the canvas / a node.
struct WeaponRow { RECT rect{}; std::string id; bool shaqi=false,header=false; std::wstring name; int image=-1; };
std::vector<WeaponRow> weapon_rows_; RECT weapons_{},weapons_title_{};
bool IsShaqiId(const std::string& id) { return ComboWeapons::IsShaqi(id); }
std::wstring WeaponName(const std::string& id) {
    const std::wstring wide(id.begin(),id.end());
    const WeaponIcons::Info info=WeaponIcons::Find(wide.c_str(),IsShaqiId(id));
    if(!info.name)return wide;
    for(const auto& other:ComboWeaponsData::kWeapons){if(id==other.id||!other.count)continue;const std::wstring other_id(other.id,other.id+std::strlen(other.id));
        const auto other_info=WeaponIcons::Find(other_id.c_str(),other.shaqi);if(other_info.name&&!std::wcscmp(info.name,other_info.name))return std::wstring(info.name)+L" · "+wide;}
    return info.name;
}
// Same display name can hide a different granted move set or backpack ID.
// Keep every native ID; WeaponName disambiguates variants with the same label.
std::vector<std::pair<std::string,bool>> ListedWeapons() {
    std::vector<std::pair<std::string,bool>> out;
    for (const bool shaqi:{false,true})
        for (const ComboWeapons::Weapon& w:ComboWeaponsData::kWeapons) {
            if (w.shaqi!=shaqi||!w.count) continue;
            out.push_back({w.id,w.shaqi});
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
    const std::wstring name=(ComboWeapons::HeldAttack(ability)?Tr(L"长按 / 蓄力 · "):L"")+MoveText(ability);
    if (!move||!move->stage[0]) return name;
    std::wstring presses=ComboWeapons::StagePresses(move->stage);
    if (UiI18n::GetLanguage()==UiI18n::Language::English) for (wchar_t& c:presses) c=c==L'重'?L'H':L'L';
    return presses+L" · "+name;
}
std::wstring DamageText(int percent) { wchar_t text[24]{}; _snwprintf_s(text,_countof(text),_TRUNCATE,L"%d%%",percent); return text; }
// A 处决 / 简易处决 trigger: whose executions it takes (any weapon or one).
bool ExecutionTrigger(const Node& node) { return node.type==NodeType::Trigger&&NpcCombo::WeaponTrigger(node.trigger); }
// A 武器派生修改, or a 空中派生 (one with air_set: its rows take the presses made in the air, no weapon).
bool WeaponActionsNode(const Node& node) { return node.type==NodeType::WeaponActions; }
// A 自定义组合键 / 自定义按键 trigger: its keys (field2) and how they are pressed (field3); 自定义按键 also whether its
// keys' game actions are swallowed (toggle2).
bool KeysTrigger(const Node& node) { return NpcCombo::IsComboTrigger(node)||NpcCombo::IsKeysTrigger(node); }
bool RawTrigger(const Node& node) { return NpcCombo::IsKeysTrigger(node); }
// 自定义按键 recording: keyboard, all five mouse buttons and pad presses edit a draft for the node (or
// its branch row `route`, -1: the node's own keys).
// route kRecordThen: one 继续派生 key. Enter commits; Esc discards every kind of draft.
constexpr int kRecordThen=-5;
struct Recording { int node=-1; int route=-1; unsigned pad_was=0; bool pad_seeded=false;
    std::vector<int> draft; const std::vector<int>* target=nullptr; uint64_t generation=0; };
Recording record_;
const std::vector<int>& DisplayKeys(const std::vector<int>& keys){return record_.node>=0&&record_.target==&keys?record_.draft:keys;}
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
    case VK_LBUTTON: return Tr(L"鼠标左键");
    case VK_RBUTTON: return Tr(L"鼠标右键");
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
    if (recording) return Tr(L"继续派生：")+(record_.draft.empty()?Tr(L"按下一个键…"):RawKeyLabel(record_.draft.front()))+Tr(L"（回车确认，Esc 取消）");
    if (!NpcCombo::HasThen(node)) return Tr(RawTrigger(node)?L"继续派生：再按原按键":L"继续派生：再按原组合键");
    const int key=NpcCombo::ThenRawKey(node);
    return Tr(L"继续派生：")+(key>=0?RawKeyLabel(key):ButtonLabel(node.then));
}
// Keys as icons: keyboard and mouse keys as keycaps, pad buttons as the game's prompt icons for the pad in use, the
// left stick's ways as its L3 icon with an arrow, a 自定义组合键's hero buttons as keycaps with their names; "+"
// (together) or "→" (in order) between two. Heights and widths in pixels.
constexpr int kKeyIcon=18;  // icon height in canvas units (a field is kRow-4 high)
bool PsPad() { return UiHotkeys::ActivePad()==UiHotkeys::Device::PlayStation; }
bool MouseKey(int code) { return code==VK_LBUTTON||code==VK_RBUTTON||code==VK_MBUTTON||code==VK_XBUTTON1||code==VK_XBUTTON2; }
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
void KeysField(HDC dc,const RECT& inner,const std::vector<int>& stored_keys,bool chord,bool raw,bool recording,bool set,UiTheme::Font font,COLORREF text,COLORREF muted) {
    const auto& keys=DisplayKeys(stored_keys);
    const size_t least=raw?1:NpcCombo::kComboKeysMin;
    if (keys.empty()||keys.size()<least) {
        const std::wstring hint=recording?Tr(L"录制中：键盘、鼠标或手柄（回车确认，Esc 取消）")+(keys.empty()?L"":L" · "+KeysText(keys,chord,raw)):KeysText(keys,chord,raw);
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
int KeyIconsUnits(HDC dc,const std::vector<int>& stored_keys,bool raw,bool recording,UiTheme::Font font) {
    const auto& keys=DisplayKeys(stored_keys);
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
std::wstring RouteModeText(const NpcCombo::Route& route,bool weapon=false) {
    std::wstring text=weapon&&route.chord&&route.keys.size()>1?Tr(L"一起按 · 可先按住格挡"):KeysModeText(route.keys.size(),route.chord,route.window,RawRoute(route));
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
    if (NpcCombo::HasRows(n)&&route>=0&&static_cast<size_t>(route)<n.routes.size()&&(part==1||part==2)&&NpcCombo::KeysRoute(n.routes[static_cast<size_t>(route)])) {
        NpcCombo::Route& r=n.routes[static_cast<size_t>(route)];
        k.keys=&r.keys; k.chord=&r.chord; k.window=&r.window; k.swallow=&r.swallow; k.raw=RawRoute(r); k.row=true; k.what=part;
    }
    return k;
}
constexpr int kKeyWindowPresets[]={200,300,400,500,700,1000,1500};
std::wstring ExecutionWeaponText(const Node& node) { return Tr(L"武器：")+(node.weapon.empty()?Tr(L"任意武器"):WeaponName(node.weapon)); }
// A 武器派生修改's weapon field.
std::wstring ActionsWeaponText(const Node& node) {
    if (node.air_set) return Tr(L"主角在空中时");
    return node.weapon.empty()?Tr(L"选择武器"):Tr(L"武器：")+WeaponName(node.weapon);
}
void Invalidate() { DropCanvasCache(); Repaint(); }
// The trigger whose chain reaches node `index` (NpcCombo::TriggerOf for every node at once; -1: none).
int ChainOf(size_t index) {
    if (owners_.size()!=config_.chain.size()) {
        owners_=NpcCombo::Owners(config_);
    }
    return index<owners_.size()?owners_[index]:-1;
}
void Layout();
void CloseCurve(const char* why);
void ScrollStrip(int delta);
const Node* At(int index) { return index>=0&&static_cast<size_t>(index)<config_.chain.size()?&config_.chain[static_cast<size_t>(index)]:nullptr; }
bool IsKey(int index) { const Node* n=At(index); return n&&n->type==NodeType::Trigger&&n->trigger==static_cast<int>(Trigger::NpcKey); }
// 预输入: the chain's fixed setting card (never removed, no pins).
bool IsPreInputAt(int index) { const Node* n=At(index); return n&&NpcCombo::IsPreInput(*n); }
bool IsSettingAt(int index) { const Node* n=At(index); return n&&NpcCombo::IsSetting(*n); }
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
        if (ChainOf(static_cast<size_t>(at))>=0) return true;
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
const std::vector<size_t>& PaintOrder() {
    if(canvas_cache_.order.size()==config_.chain.size())return canvas_cache_.order;
    std::vector<size_t> frames,others;
    for (size_t i=0;i<config_.chain.size();++i) (IsFrame(static_cast<int>(i))?frames:others).push_back(i);
    std::stable_sort(frames.begin(),frames.end(),[](size_t a,size_t b){ return NpcCombo::GroupDepth(config_,static_cast<int>(a))<NpcCombo::GroupDepth(config_,static_cast<int>(b)); });
    frames.insert(frames.end(),others.begin(),others.end());
    canvas_cache_.order=std::move(frames);return canvas_cache_.order;
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
    case NodeType::BoneTransform: return Tr(L"骨骼变换");
    case NodeType::Weapon: return Tr(L"武器属性");
    case NodeType::Armor: return Tr(L"霸体");
    case NodeType::Invincible: return Tr(L"无敌");
    case NodeType::IgnoreGuard: return Tr(L"无视防御");
    case NodeType::KeepChain: return Tr(L"保持派生");
    case NodeType::KeepLook: return Tr(L"保留变身");
    case NodeType::ForceHit: return Tr(L"强制受击");
    case NodeType::Interrupt: return Tr(L"允许中断");
    case NodeType::Grab: return Tr(L"投技");
    case NodeType::BossSkill: return Tr(L"BOSS 技能");
    case NodeType::Assist: return Tr(L"BOSS 援护");
    case NodeType::WeaponMove: return Tr(L"武器派生");
    case NodeType::Damage: return Tr(L"伤害倍率");
    case NodeType::Teleport: return Tr(L"瞬移");
    case NodeType::Group: return Tr(L"组合");
    case NodeType::Look: return Tr(L"变身");
    case NodeType::HeroLook: return Tr(L"变回主角");
    case NodeType::PreInput: return Tr(L"预输入");
    case NodeType::WeaponActions: return Tr(node.air_set?L"空中派生":L"武器派生修改");
    case NodeType::Hook: return Tr(L"钩子");
    case NodeType::Trim: return Tr(L"裁剪");
    case NodeType::Air: return Tr(L"滞空");
    case NodeType::BossEffect: return Tr(L"BOSS 特效");
    case NodeType::AirDefault: return Tr(L"空中释放");
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
// BOSS 特效's places and fields (route -1 source, -2 effect, -3 place, -4 damage, -5 radius, -6 hits).
std::wstring FxPlaceText(int place) {
    switch (static_cast<NpcCombo::EffectPlace>(NpcCombo::ClampEffectPlace(place))) {
    case NpcCombo::EffectPlace::Weapon: return Tr(L"武器上（跟随武器）");
    case NpcCombo::EffectPlace::Hero: return Tr(L"主角脚下（跟随主角）");
    case NpcCombo::EffectPlace::Front: return Tr(L"主角前方 3 米（锁定时朝向目标）");
    default: return Tr(L"锁定目标身上（跟随；未锁定：最近的敌人）");
    }
}
// A BOSS 特效 folder as the page names it: the look's name when the folder is a look's id, else its alias
// (NpcBossEffects::FolderAlias), else the id.
std::wstring FxFolderName(const std::string& folder) {
    if (const wchar_t* alias=NpcBossEffects::FolderAlias(folder)) return alias;
    static std::unordered_map<std::string,std::wstring> looks;  // every look's id -> name (static data: offline too)
    if (looks.empty()) { NpcRetarget::SkinMoves m; for (int32_t row=0;row<1024&&NpcRetarget::GetSkinMoves(row,m);++row) looks.emplace(m.profile,m.name); }
    if (const auto it=looks.find(folder);it!=looks.end()) return it->second;
    return std::wstring(folder.begin(),folder.end());
}
std::wstring FxFieldText(const Node& node,int route) {
    wchar_t text[96]{};
    switch (route) {
    case -1: return node.boss.empty()?Tr(L"选择特效来源"):Tr(L"来源：")+FxFolderName(node.boss);
    case -2: {
        const NpcBossEffect* e=NpcBossEffects::Find(node.ability);
        if (!e) return node.ability.empty()?Tr(L"选择特效"):Tr(L"（这个版本没有此特效）");
        const std::string n=NpcBossEffects::ShortName(*e); return Tr(L"特效：")+std::wstring(n.begin(),n.end());
    }
    case -3: return Tr(L"位置：")+FxPlaceText(node.fx_place);
    case -4:
        if (NpcCombo::ClampEffectDamage(node.fx_damage)==0) return Tr(L"伤害：无（只放特效）");
        _snwprintf_s(text,_countof(text),_TRUNCATE,Tr(L"伤害：每段 %d%% 最大生命").c_str(),NpcCombo::ClampEffectDamage(node.fx_damage)); return text;
    case -5: _snwprintf_s(text,_countof(text),_TRUNCATE,Tr(L"范围：半径 %.1f 米").c_str(),NpcCombo::ClampEffectRadius(node.fx_radius)/100.0); return text;
    default: _snwprintf_s(text,_countof(text),_TRUNCATE,Tr(L"段数：%d 段（每 0.3 秒一段）").c_str(),NpcCombo::ClampEffectHits(node.fx_hits)); return text;
    }
}
// 派生页的技能伤害: "×2", "×1（Boss 原伤害）".
std::wstring ChainSkillText(float v) {
    wchar_t text[64]{};
    if (std::fabs(v-1.f)<1e-3f) _snwprintf_s(text,_countof(text),_TRUNCATE,Tr(L"×%g（Boss 原伤害）").c_str(),static_cast<double>(v));
    else _snwprintf_s(text,_countof(text),_TRUNCATE,L"×%g",static_cast<double>(v));
    return text;
}
// 派生页的投技伤害: "原生伤害", "×4.5", "每段至少 30% 最大生命".
std::wstring ChainGrabText(const NpcCombo::ChainGrab& g) {
    if (g.mode==NpcCombo::kGrabFollow) return Tr(L"跟随投技页");
    if (g.mode==0) return Tr(L"原生伤害");
    wchar_t text[64]{};
    if (g.mode==1) _snwprintf_s(text,_countof(text),_TRUNCATE,L"×%g",static_cast<double>(g.value));
    else _snwprintf_s(text,_countof(text),_TRUNCATE,Tr(L"每段至少 %g%% 最大生命").c_str(),static_cast<double>(g.value));
    return text;
}
// 按住: "点按" (any press) or how long the button must be held.
std::wstring HoldText(int ms) {
    if (NpcCombo::ClampHold(ms)<=0) return Tr(L"点按");
    wchar_t text[48]{}; _snwprintf_s(text,_countof(text),_TRUNCATE,Tr(L"按住 %.1f s").c_str(),NpcCombo::ClampHold(ms)/1000.0); return text;
}
// A shared trim follows the playing linked skill, then selection/preview, then
// its first owner. The displayed source identifies which montage length is used.
int TrimSource(size_t index){
    const auto linked=[&](int source){return At(source)&&NpcCombo::TakesTrim(*At(source))&&NpcCombo::ModOf(config_,source,NpcCombo::kModTrim)==static_cast<int>(index);};
    if(run_.running&&run_.profile==moves_.profile&&linked(run_.node))return run_.node;
    if(linked(selected_))return selected_;
    if(linked(preview_.node))return preview_.node;
    return NpcCombo::TrimmedBy(config_,static_cast<int>(index));
}
float TrimClip(size_t index) {const Node* source=At(TrimSource(index));return source?NpcCombo::Clip(NpcCombo::DurationKey(*source,moves_.profile)):0.f;}
std::wstring TrimEndText(const Node& node) { return node.trim_to>0?Seconds(NpcCombo::ClampTrim(node.trim_to)/1000.0):Tr(L"放完"); }
std::wstring TrimSourceText(size_t index){
    const Node* source=At(TrimSource(index));if(!source)return Tr(L"未连接技能");
    const std::string short_name=NpcCombo::ShortName(source->ability);
    return Tr(L"来源：")+std::wstring(short_name.begin(),short_name.end());
}
// Match actual playback: a start outside this montage leaves the whole move.
std::wstring TrimBarText(size_t index) {
    const float clip=TrimClip(index);
    if (!(clip>0)) return Tr(TrimSource(index)<0?L"裁后时长：未连接技能":L"裁后时长：待读取原始长度");
    NpcCombo::TrimRange r=NpcCombo::TrimOf(config_.chain[index]); if (r.from>=clip) r={};
    const double span=NpcCombo::TrimSpan(r,clip);
    wchar_t text[128]{}; _snwprintf_s(text,_countof(text),_TRUNCATE,Tr(L"裁后 %.2f s · %.2f–%.2f s").c_str(),span,r.from,r.from+span);
    return text;
}
// 钩子 modes: 0 勾过来, 1 勾过去, 2 自动 (the target's guard picks it when it plays).
std::wstring HookModeText(int mode) {
    return Tr(mode==2?L"自动：未破防勾过去，破防勾过来":mode==1?L"勾过去：把自己拉到目标处":L"勾过来：把目标拉到身前");
}
int HookModeOf(const Node& node) { return node.hook_auto?2:node.hook_self?1:0; }
Node NewHook() { Node node=NpcCombo::HookNode(0,0); NpcCombo::SetHookAuto(node); return node; }
// 钩子's reach (shown in metres) and speed (a multiplier), with a few values to pick.
std::wstring HookDistText(int cm) { wchar_t text[48]{}; _snwprintf_s(text,_countof(text),_TRUNCATE,Tr(L"最远 %.1f 米").c_str(),NpcCombo::ClampHookDist(cm)/100.0); return text; }
std::wstring HookSpeedText(int percent) { wchar_t text[48]{}; _snwprintf_s(text,_countof(text),_TRUNCATE,Tr(L"钩子速度 ×%.2f").c_str(),NpcCombo::ClampHookSpeed(percent)/100.0); return text; }
constexpr int kHookDistPresets[]={500,1000,1500,2000,3000},kHookSpeedPresets[]={50,75,100,150,200,300};
// 滞空 / 空中释放: field 1 the hero's lift, 2 its hang, 3 the enemies' lift, 4 theirs hang (routes -1 .. -4); lifts in
// metres, hangs in percent of the gravity taken away. 强制受击 击飞: field 2 its height.
bool AirNode(const Node& node) { return node.type==NodeType::Air||node.type==NodeType::AirDefault; }
bool AirHangRoute(int route) { return route==-2||route==-4; }
int& AirValue(Node& node,int route) { return route==-2?node.air_hang:route==-3?node.air_foe_lift:route==-4?node.air_foe_hang:node.air_lift; }
int AirValueOf(const Node& node,int route) { return AirValue(const_cast<Node&>(node),route); }
std::wstring AirText(int route,int value) {
    wchar_t text[64]{};
    if (AirHangRoute(route)) _snwprintf_s(text,_countof(text),_TRUNCATE,Tr(route==-2?L"自身滞空 %d%%":L"敌人滞空 %d%%").c_str(),NpcCombo::ClampAirHang(value));
    else _snwprintf_s(text,_countof(text),_TRUNCATE,Tr(route==-1?L"自身升高 %.1f 米":L"敌人升高 %.1f 米").c_str(),NpcCombo::ClampAirLift(value)/100.0);
    return text;
}
std::wstring LaunchText(int cm) { wchar_t text[48]{}; _snwprintf_s(text,_countof(text),_TRUNCATE,Tr(L"击飞高度 %.1f 米").c_str(),NpcCombo::ClampLaunch(cm)/100.0); return text; }
bool LaunchShown(const Node& node) { return node.type==NodeType::ForceHit&&node.hit==static_cast<int>(NpcCombo::HitKind::Launch); }
constexpr int kAirLiftPresets[]={0,50,100,150,200,300,500,800},kAirHangPresets[]={0,25,50,70,85,100},kLaunchPresets[]={100,200,300,500,800,1200};
std::wstring PauseText(int ms) {
    if (ms<=0) return Tr(L"直接按");
    wchar_t text[48]{}; _snwprintf_s(text,_countof(text),_TRUNCATE,Tr(L"停 %.1f s 后").c_str(),ms/1000.0); return text;
}
// A 按住 / pause value box as wide as the widest value it can show (no "…", no width change while stepping), and a
// − value + stepper around such a box, in canvas units.
int HoldValueUnits(HDC dc,UiTheme::Font font) { return (std::max)(Units(dc,font,HoldText(NpcCombo::kHoldMax)),Units(dc,font,Tr(L"点按")))+16; }
int PauseValueUnits(HDC dc,UiTheme::Font font) { return (std::max)(Units(dc,font,PauseText(NpcCombo::kPauseMax)),Units(dc,font,Tr(L"直接按")))+16; }
int StepperUnits(int value) { return 2*(kRow-4)+8+value; }
int TriggerHoldUnits(HDC dc) { return StepperUnits((std::max)(62,HoldValueUnits(dc,UiTheme::Font::Strong))); }
int RouteHoldUnits(HDC dc) { return (std::max)(84,HoldValueUnits(dc,UiTheme::Font::Small)); }
int RoutePauseUnits(HDC dc) { return (std::max)(96,PauseValueUnits(dc,UiTheme::Font::Small)); }
std::wstring SpeedText(int percent) { wchar_t text[24]{}; _snwprintf_s(text,_countof(text),_TRUNCATE,L"×%.2f",percent/100.0); return text; }
// The node an exec wire comes into `index` from (-1: none).
int Feeder(int index) {
    for (size_t i=0;i<config_.chain.size();++i) if (NpcCombo::Feeds(config_.chain[i],index)) return static_cast<int>(i);
    return -1;
}
// A move's speed in percent (its own 加速 nodes, else its chain's).
int MoveSpeed(size_t index) { return NpcCombo::SpeedFor(config_,ChainOf(index),static_cast<int>(index)); }
// An interval waits after the preceding node's derivation gate; it never
// stretches that node's animation to fill its own duration.
std::wstring IntervalNote(size_t index) {
    return Tr(Wired(index)?L"前招到派生点（未设则放完）后开始等待，不改变前招速度":L"未接入派生链");
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
    if (node.type==NodeType::Trim) {
        if (!from.nodes) return Tr(L"未连接：从技能、武器派生或闪避拉箭头到这里");
        const NpcCombo::TrimRange r=NpcCombo::TrimOf(node); const float clip=TrimClip(index);
        if (node.trim_to>0&&node.trim_to<=node.trim_from) return Tr(L"结束不晚于开始：放到这一招结尾");
        if (clip>0&&r.from>=clip) { wchar_t text[128]{}; _snwprintf_s(text,_countof(text),_TRUNCATE,Tr(L"开始晚于这一招的长度 %.2f s：整招照常播放").c_str(),static_cast<double>(clip)); return text; }
        if (clip>0) return Tr(L"右键增删停留点 · 预览按住空格");
        if (!r.on) return Tr(L"从头放到结尾：调开始或结束来裁剪");
        wchar_t text[96]{}; _snwprintf_s(text,_countof(text),_TRUNCATE,Tr(L"指向它的 %d 招技能只播放这一段").c_str(),from.nodes); return text;
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
    if (node.type==NodeType::BossEffect) {
        if (node.ability.empty()) return Tr(L"先选择特效");
        if (from.triggers) return Tr(L"整条链：每一招开始时放这个特效");
        wchar_t text[128]{}; _snwprintf_s(text,_countof(text),_TRUNCATE,Tr(L"指向它的 %d 招开始时放这个特效").c_str(),from.nodes); return text;
    }
    if (node.type==NodeType::Air) {
        if (from.triggers) return Tr(L"整条链：主角和打中的敌人按这里升高、滞空");
        wchar_t text[128]{}; _snwprintf_s(text,_countof(text),_TRUNCATE,Tr(L"指向它的 %d 招：主角和打中的敌人按这里升高、滞空").c_str(),from.nodes); return text;
    }
    if (node.type==NodeType::IgnoreGuard) {
        if (from.triggers) return Tr(L"整条链无视防御：敌人格挡、弹反不了");
        wchar_t text[96]{}; _snwprintf_s(text,_countof(text),_TRUNCATE,Tr(L"指向它的 %d 招无视防御").c_str(),from.nodes); return text;
    }
    if (node.type==NodeType::KeepChain) {
        if (from.triggers) return Tr(L"整条链：闪避、移动等任何动作都不打断派生，按这个时间等下一次按键");
        wchar_t text[128]{}; _snwprintf_s(text,_countof(text),_TRUNCATE,Tr(L"指向它的 %d 招后：任何动作都不打断派生").c_str(),from.nodes); return text;
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
// A chain start's 整体不变身 row, and the hint a BOSS 技能 / 投技 card shows when its starts play it 不变身.
std::wstring ChainOwnLookText(bool on) {
    return Tr(on?L"整体不变身：这条链的 BOSS 技能、投技都不换外观":L"整体不变身（这条链的 BOSS 技能、投技）");
}
bool ChainOwnLook(size_t index) {
    return index<config_.chain.size()&&!config_.chain[index].own_look&&NpcCombo::OwnLookEverywhere(config_,index);
}
std::wstring OwnLookText(size_t index) {
    const Node& node=config_.chain[index];
    if (ChainOwnLook(index)) return Tr(L"不变身：起端已设整体不变身");
    return Tr(node.own_look?L"不变身：动作套到当前外观上":L"不变身（动作套到主角）");
}
std::wstring GrabNote(size_t index) {
    const Node& node=config_.chain[index];
    if (node.ability.empty()) return Tr(L"选一个投技；没选时按键跳过这个节点");
    const MotianGrab::Variant* v=NodeGrab(node);
    if (!v) return Tr(L"所选投技已不可用，请重新选择");
    // The hero takes the look of the grab's boss for it (none when no look plays that boss).
    const bool own=node.own_look||NpcCombo::OwnLookEverywhere(config_,index);  // its own 不变身, or its starts' 整体不变身
    const std::wstring look=Tr(own?L"不变身":NpcRetarget::SkinForGrab(v->boss_id,v->source)>=0?L"投时变身为该 Boss":L"这个 Boss 没有外观，不变身");
    const MotianGrab::Snapshot& grab=GrabState();
    if (grab.chain_variant==v->index) return Tr(grab.chain_prepared?L"已预载":L"预载中")+L" · "+look;
    return look+L" · "+Tr(L"不受投技页设置影响");
}
std::wstring NodeNote(size_t index) {
    const Node& node=config_.chain[index];
    const bool wired=Wired(index);
    switch (node.type) {
    case NodeType::Teleport: return wired?Tr(L"瞬身到锁定目标面前（未锁定：最近的敌人）"):Tr(L"未连接");
    case NodeType::Hook: return wired?Tr(L"钩向瞄准处：锁定目标、准星下的敌人，或地面和墙壁（飞过去）"):Tr(L"未连接");
    case NodeType::Group: {
        const size_t steps=NpcCombo::GroupSteps(config_,static_cast<int>(index)).size();
        if (!steps) return Tr(L"把节点拖进框里：按顺序直接连续释放");
        return wired?Tr(L"框里的招按顺序直接连续释放"):Tr(L"未连接");
    }
    case NodeType::Interval: return IntervalNote(index);
    case NodeType::AutoDodge: return moves_.dodges.empty()?Tr(L"这个外观没有闪避"):wired?Tr(L"按摇杆方向选 Boss 闪避"):Tr(L"未连接");
    case NodeType::Trigger: return {};
    case NodeType::Branch: return Tr(L"上一招后按哪个键（点按或按住），接哪一招");
    case NodeType::Speed: case NodeType::Weapon: case NodeType::Armor: case NodeType::Interrupt: case NodeType::Damage: case NodeType::Invincible: case NodeType::KeepLook: case NodeType::ForceHit: case NodeType::IgnoreGuard: case NodeType::KeepChain: case NodeType::Trim: case NodeType::Air: case NodeType::BossEffect: return ModifierNote(index);
    case NodeType::WeaponMove: {
        if (!wired) return Tr(L"未连接");
        return Tr(L"拿这把武器放这一招，放完换回");
    }
    case NodeType::Grab: return wired?GrabNote(index):GrabNote(index)+L" · "+Tr(L"未连接");
    case NodeType::BossSkill: return wired?Tr(node.own_look||NpcCombo::OwnLookEverywhere(config_,index)?L"在当前外观上放这一招（不变身）":L"瞬身变身放这一招，放完变回"):Tr(L"未连接");
    case NodeType::Assist: return wired?Tr(L"Boss 瞬移到目标身前放这一招，主角照常行动"):Tr(L"未连接");
    case NodeType::Look:
        if (node.boss.empty()) return Tr(L"选择要变成的 Boss");
        return wired?Tr(L"瞬身变成这个 Boss，派生链结束后保持"):Tr(L"未连接");
    case NodeType::HeroLook: return wired?Tr(L"瞬身变回主角，派生链结束后保持"):Tr(L"未连接");
    case NodeType::PreInput: return Tr(node.ms>0?L"招式中提前按的键保留这么久，没接上就清除":L"提前按的键一直保留到接上招式");
    case NodeType::BoneTransform: return node.bones.on?L"已启用 · "+std::to_wstring(node.bones.rows.size())+L" 根骨骼":L"未启用 · 点击详细编辑";
    case NodeType::AirDefault: return Tr(node.on?L"空中放出、没连滞空的招式按这里升高、滞空":L"已关闭：空中放招不额外升高、滞空");
    case NodeType::WeaponActions:
        if (node.air_set) return Tr(L"空中点按 / 按住下面的键，改放连接的招式");
        return Tr(node.weapon.empty()?L"先选择武器":L"点按 / 按住分开连线；同键优先匹配已达到的最长时间");
    default: break;
    }
    if (node.ability.empty()||!MoveOfSkin(node.ability)) return Tr(L"不属于当前外观");
    std::wstring note=Tr(NpcCombo::KindLabel(KindOf(node.ability)));
    const int speed=MoveSpeed(index);
    if (speed!=100) note+=L" · "+SpeedText(speed);
    if (!wired) note+=L" · "+Tr(L"未连接");
    return note;
}
// Width and height in canvas units (measured: NodeSize keeps them).
SIZE MeasureNodeBody(HDC dc,size_t index) {
    ++canvas_cache_.measurements;
    const Node& node=config_.chain[index];
    if (node.type==NodeType::Trigger) {
        const bool key=node.trigger==static_cast<int>(Trigger::NpcKey),execution=ExecutionTrigger(node);
        int field=(key?KeyUnits(dc):Units(dc,UiTheme::Font::Body,ButtonLabel(node.trigger)))+100;
        if (execution) field=(std::max)(field,Units(dc,UiTheme::Font::Body,ExecutionWeaponText(node)+L"  ▾")+60);
        // The 按住 row: its label, then its stepper with the whole value in view.
        const int hold=(std::max)(10+Units(dc,UiTheme::Font::Small,Tr(L"按住多久才触发"))+6+TriggerHoldUnits(dc)+10,
                                  Units(dc,UiTheme::Font::Small,ChainOwnLookText(true))+50);  // + 整体不变身
        if (KeysTrigger(node)) {
            // The keys as icons: the card grows with them (up to six keys of any kind).
            const bool recording=record_.node==static_cast<int>(index)&&record_.route==-1;
            const bool then_recording=record_.node==static_cast<int>(index)&&record_.route==kRecordThen;
            const int icons=KeyIconsUnits(dc,node.keys,RawTrigger(node),recording,UiTheme::Font::Body);
            const int keys=icons>0?icons:Units(dc,UiTheme::Font::Body,KeysText(node)+L"  ▾");
            // The keys field is the row less 26 units each side and 7 px inside: icons need that much more.
            field=(std::max)(field,(std::max)(keys+(icons>0?70:60),Units(dc,UiTheme::Font::Body,KeysModeText(node)+L"  ▾")+60));
            field=(std::max)(field,Units(dc,UiTheme::Font::Body,ThenText(node,then_recording)+L"  ▾")+60);  // 继续派生
            return {std::clamp((std::max)(field,hold),300,(std::max)(icons>0?900:480,hold)),kHeader+kRow*(RawTrigger(node)?9:8)+kPad};  // + 按住, 整体不变身
        }
        return {std::clamp((std::max)(field,hold),260,(std::max)(350,hold)),kHeader+kRow*(key?4:execution?6:5)+kPad};  // + 按住, 整体不变身
    }
    if (node.type==NodeType::Branch) {
        // A row each; a 自定义组合键 / 自定义按键 row two (its keys under it).
        int lines=0; int wide=500; bool icons=false;  // a row: its button, 按住, pause, x
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
        // A row: its button, its 按住 and pause steppers (each value whole), x, its output.
        const int row=10+132+8+StepperUnits(RouteHoldUnits(dc))+8+StepperUnits(RoutePauseUnits(dc))+8+(kRow-4)+30;
        return {std::clamp((std::max)(wide,row),500,(std::max)(icons?960:660,row)),kHeader+kRow*(lines+2)+kPad};
    }
    if (node.type==NodeType::WeaponActions) {
        // The weapon, the reset time, a note, a row per action (its field, x, its own output), then "+ 添加动作".
        int wide=(std::max)(Units(dc,UiTheme::Font::Strong,CardTitle(index))+70,(std::max)(Units(dc,UiTheme::Font::Body,ActionsWeaponText(node)+L"  ▾")+60,
                                                                                   Units(dc,UiTheme::Font::Small,NodeNote(index))+34));
        wide=(std::max)(wide,Units(dc,UiTheme::Font::Small,ChainOwnLookText(true))+50);  // 整体不变身
        int extra=0;for (const NpcCombo::Route& route:node.routes){wide=(std::max)(wide,Units(dc,UiTheme::Font::Small,ButtonLabel(route.button)+L" ▾")+116+StepperUnits(RouteHoldUnits(dc)));
            if(NpcCombo::KeysRoute(route)){++extra;wide=(std::max)(wide,KeyIconsUnits(dc,route.keys,RawRoute(route),false,UiTheme::Font::Small)+Units(dc,UiTheme::Font::Small,RouteModeText(route,true))+100);}}
        return {std::clamp(wide,500,900),kHeader+kRow*(static_cast<int>(node.routes.size())+extra+5)+kPad};  // + 整体不变身
    }
    if (node.type==NodeType::Group) return {NpcCombo::ClampGroupW(node.w),NpcCombo::ClampGroupH(node.h)};
    const int title=Units(dc,UiTheme::Font::Strong,CardTitle(index))+70,note=Units(dc,UiTheme::Font::Small,NodeNote(index))+34;
    if (node.type==NodeType::Hook) {  // its mode, its reach, its speed, a note, its 派生点
        int field=0; for (int mode=0;mode<3;++mode) field=(std::max)(field,Units(dc,UiTheme::Font::Body,HookModeText(mode)+L"  ▾")+60);
        field=(std::max)(field,(std::max)(Units(dc,UiTheme::Font::Body,HookDistText(NpcCombo::kHookDistMax)+L"  ▾"),Units(dc,UiTheme::Font::Body,HookSpeedText(NpcCombo::kHookSpeedMax)+L"  ▾"))+60);
        return {std::clamp((std::max)((std::max)(title,field),note),300,520),kHeader+kRow*5+kPad};
    }
    if (node.type==NodeType::Teleport) {
        const int fields=Units(dc,UiTheme::Font::Body,DistText(NpcCombo::kTeleportDistMax)+L"  ▾")+60;
        return {std::clamp((std::max)((std::max)(title,fields),note),280,420),kHeader+kRow*4+kPad};
    }
    if (node.type==NodeType::Trim) {  // 开始于, 结束于 (steppers), the part on the move's length, a note
        const int labels=(std::max)(Units(dc,UiTheme::Font::Small,Tr(L"开始于")),Units(dc,UiTheme::Font::Small,Tr(L"结束于")))+150;
        const int bar=Units(dc,UiTheme::Font::Small,TrimBarText(index))+40;
        return {std::clamp((std::max)((std::max)(title,note),(std::max)(labels,bar)),300,460),kHeader+kRow*6+kPad};
    }
    if(node.type==NodeType::BoneTransform)return {320,kHeader+kRow*3+kPad};
    if (node.type==NodeType::Speed) {
        const int curve=Units(dc,UiTheme::Font::Small,Tr(L"曲线调节"))+Units(dc,UiTheme::Font::Small,Tr(L"编辑曲线…"))+80;
        return {std::clamp((std::max)((std::max)(title,note),curve),240,360),kHeader+kRow*3+kPad};
    }
    if (node.type==NodeType::Damage) return {std::clamp((std::max)(title,note),200,340),kHeader+kRow*2+kPad};
    if (node.type==NodeType::KeepChain) return {std::clamp((std::max)(title,note),220,420),kHeader+kRow*2+kPad};
    if (node.type==NodeType::WeaponMove) {
        const int weapon=Units(dc,UiTheme::Font::Body,(node.weapon.empty()?Tr(L"选择武器"):WeaponName(node.weapon))+L"  ▾")+60;
        const int move=Units(dc,UiTheme::Font::Body,(node.ability.empty()?Tr(L"选择招式"):WeaponMoveText(node.weapon,node.ability))+L"  ▾")+60;
        return {std::clamp((std::max)((std::max)(title,weapon),(std::max)(move,note)),280,440),kHeader+kRow*4+kPad};
    }
    if (node.type==NodeType::Armor||node.type==NodeType::Interrupt||node.type==NodeType::Invincible||node.type==NodeType::KeepLook||node.type==NodeType::IgnoreGuard) return {std::clamp((std::max)(title,note),200,340),kHeader+kRow+kPad};
    if (node.type==NodeType::BossSkill) {
        const int look=Units(dc,UiTheme::Font::Body,(node.boss.empty()?Tr(L"选择外观"):BossLookName(node.boss))+L"  ▾")+60;
        const int move=Units(dc,UiTheme::Font::Body,(node.ability.empty()?Tr(L"选择招式"):MoveText(node.ability))+L"  ▾")+60;
        return {std::clamp((std::max)((std::max)(title,look),move),280,420),kHeader+kRow*4+kPad};  // + 不变身
    }
    if (node.type==NodeType::Assist) {  // its look, its move, a note
        const int look=Units(dc,UiTheme::Font::Body,(node.boss.empty()?Tr(L"选择外观"):BossLookName(node.boss))+L"  ▾")+60;
        const int move=Units(dc,UiTheme::Font::Body,(node.ability.empty()?Tr(L"选择招式"):MoveText(node.ability))+L"  ▾")+60;
        return {std::clamp((std::max)((std::max)(title,look),(std::max)(move,note)),280,460),kHeader+kRow*3+kPad};
    }
    if (node.type==NodeType::Look) {
        const int field=Units(dc,UiTheme::Font::Body,(node.boss.empty()?Tr(L"选择外观"):BossLookName(node.boss))+L"  ▾")+60;
        return {std::clamp((std::max)((std::max)(title,note),field),260,420),kHeader+kRow*2+kPad};
    }
    if (node.type==NodeType::HeroLook) return {std::clamp((std::max)(title,note),240,380),kHeader+kRow+kPad};
    if (node.type==NodeType::ForceHit) {  // its kind, 击飞's height, a note
        int field=Units(dc,UiTheme::Font::Body,Tr(NpcCombo::HitKindLabel(node.hit))+L"  ▾")+60;
        if (LaunchShown(node)) field=(std::max)(field,Units(dc,UiTheme::Font::Body,LaunchText(NpcCombo::kLaunchMax)+L"  ▾")+60);
        return {std::clamp((std::max)((std::max)(title,note),field),240,400),kHeader+kRow*(LaunchShown(node)?3:2)+kPad};
    }
    if (node.type==NodeType::BossEffect) {  // its source, effect, place, damage, radius and hits; a note
        int field=0;
        for (int route=-1;route>=-6;--route) field=(std::max)(field,Units(dc,UiTheme::Font::Body,FxFieldText(node,route)+L"  ▾")+90);
        return {std::clamp((std::max)((std::max)(title,note),field),280,520),kHeader+kRow*7+kPad};
    }
    if (AirNode(node)) {  // 空中释放: its switch; then the four values and a note
        int field=0;
        for (const int route:{-1,-2,-3,-4}) field=(std::max)(field,Units(dc,UiTheme::Font::Body,AirText(route,AirHangRoute(route)?100:NpcCombo::kAirLiftMax)+L"  ▾")+60);
        if (node.type==NodeType::AirDefault) field=(std::max)(field,Units(dc,UiTheme::Font::Small,Tr(L"启用"))+60);
        return {std::clamp((std::max)((std::max)(title,note),field),260,440),kHeader+kRow*(node.type==NodeType::AirDefault?6:5)+kPad};
    }
    if (node.type==NodeType::Grab) {
        const int field=Units(dc,UiTheme::Font::Body,GrabField(node)+L"  ▾")+40;
        return {std::clamp((std::max)((std::max)(title,note),field),260,420),kHeader+kRow*3+kPad};  // + 不变身
    }
    if (node.type==NodeType::Weapon) return {std::clamp((std::max)((std::max)(title,note),Units(dc,UiTheme::Font::Small,WeaponNote())+24),230,360),kHeader+kRow*2+kPad};
    if (node.type==NodeType::Interval) return {std::clamp((std::max)(title,note),190,320),kHeader+kRow*2+kPad};
    if (node.type==NodeType::PreInput) return {std::clamp((std::max)(title,note),230,420),kHeader+kRow*2+kPad};
    return {std::clamp((std::max)(title,note),250,340),kHeader+kRow*2+kPad};  // a move: its note, then its 派生点
}
std::wstring AnimationDuration(size_t index) {
    const Node& node=config_.chain[index];
    std::string key=NpcCombo::DurationKey(node,moves_.profile);
    if(node.type==NodeType::Grab){const auto* grab=NodeGrab(node);if(grab){key=MotianGrab::PreviewMontage(grab->index);const auto slash=key.find_last_of('/');if(slash!=std::string::npos)key=key.substr(slash+1);key=key.substr(0,key.find('.'));}}
    const float length=NpcCombo::Clip(key);
    if(!(length>0))return Tr(L"动画时长：待读取（播放或预览后显示）");
    auto trim=NpcCombo::TrimFor(config_,static_cast<int>(index));if(trim.from>=length)trim={};
    std::wstring text=Tr(L"动画 ")+Seconds(length);
    if(trim.on)text+=Tr(L" · 裁后 ")+Seconds(NpcCombo::TrimSpan(trim,length));
    if(node.type==NodeType::AutoDodge||node.hook_auto)text+=Tr(L" · 最近一次");
    return text;
}
SIZE MeasureNode(HDC dc,size_t index) {
    SIZE size=MeasureNodeBody(dc,index);
    if(NpcCombo::HasAnimation(config_.chain[index])){size.cy+=kRow;size.cx=(std::max)(size.cx,340L);}
    return size;
}
SIZE NodeSize(HDC dc,size_t index) {
    if (index>=config_.chain.size()) return {0,0};
    if (config_.chain[index].type==NodeType::Group) return MeasureNode(dc,index);  // its own w / h, resized live
    if (sizes_.size()!=config_.chain.size()) sizes_.assign(config_.chain.size(),SIZE{-1,-1});
    SIZE& size=sizes_[index];
    if (size.cx<0) size=MeasureNode(dc,index);
    return size;
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
    const bool modifier=NpcCombo::IsModifier(node),move=node.type==NodeType::Skill||node.type==NodeType::AutoDodge,setting=NpcCombo::IsSetting(node);
    s.has_in=!NpcCombo::StartsChain(node)&&!modifier&&!setting; s.has_out=!NpcCombo::HasRows(node)&&!modifier&&!setting;
    s.in={s.rect.left+Px(13),mid}; s.out={s.rect.right-Px(13),mid};
    // Round pins: a trigger's arrow pin sits where an exec input would, a move's just under its exec input; a
    // modifier's (where arrows end) where an exec output would.
    s.has_mod_out=NpcCombo::StartsChain(node)||move||NpcCombo::PicksBossMove(node)||node.type==NodeType::WeaponMove||node.type==NodeType::Hook||node.type==NodeType::Teleport||node.type==NodeType::Group||
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
    if (node.type==NodeType::Interval||NpcCombo::IsPreInput(node)) {
        const RECT line=Row(s.rect,0); const int b=line.bottom-line.top;
        s.minus={line.left,line.top,line.left+b,line.bottom}; s.plus={line.right-b,line.top,line.right,line.bottom};
        s.value={s.minus.right+Px(4),line.top,s.plus.left-Px(4),line.bottom};
    } else if(node.type==NodeType::BoneTransform){s.field=Row(s.rect,1);
    } else if (node.type==NodeType::Speed) {
        // Row 0: the multiplier (a stepper), or under 曲线调节 the curve's picture (a click opens its window); row 1: the
        // 曲线调节 checkbox and 编辑曲线…; row 2: the note.
        const RECT line=Row(s.rect,0); const int b=line.bottom-line.top;
        if (node.curve) s.field2=line;
        else {
            s.minus={line.left,line.top,line.left+b,line.bottom}; s.plus={line.right-b,line.top,line.right,line.bottom};
            s.value={s.minus.right+Px(4),line.top,s.plus.left-Px(4),line.bottom};
        }
        const RECT row=Row(s.rect,1); const int split=row.left+(row.right-row.left)*11/20;
        s.toggle={row.left,row.top,split,row.bottom}; s.field={split+Px(6),row.top,row.right,row.bottom};
    } else if (node.type==NodeType::Trim) {
        // Row 0: 开始于 (a stepper); row 1: 结束于 (a stepper, as a trigger's 按住); row 2: the part on the move's length;
        // row 3: kept duration/range; row 4: source skill; row 5: the note.
        const auto trim_row=[&](int row,RECT& minus,RECT& value,RECT& plus){
            const RECT line=Row(s.rect,row); const int b=line.bottom-line.top;
            minus={line.right-Px(124),line.top,line.right-Px(124)+b,line.bottom}; plus={line.right-b,line.top,line.right,line.bottom};
            value={minus.right+Px(4),line.top,plus.left-Px(4),line.bottom};
        };
        trim_row(0,s.minus,s.value,s.plus); trim_row(1,s.hminus,s.hvalue,s.hplus); s.has_hold=true;
        s.field2=Row(s.rect,2);
    } else if (node.type==NodeType::Damage||node.type==NodeType::KeepChain) {
        const RECT line=Row(s.rect,0); const int b=line.bottom-line.top;
        s.minus={line.left,line.top,line.left+b,line.bottom}; s.plus={line.right-b,line.top,line.right,line.bottom};
        s.value={s.minus.right+Px(4),line.top,s.plus.left-Px(4),line.bottom};
    } else if (node.type==NodeType::BossSkill) {
        s.field=Row(s.rect,0,26); s.field2=Row(s.rect,1,26);
        stepper(2);  // its 派生点
        s.toggle=Row(s.rect,3);  // 不变身
    } else if (node.type==NodeType::Assist) {
        s.field=Row(s.rect,0,26); s.field2=Row(s.rect,1,26);  // its look, its move; then a note (no 派生点: the hero is free)
    } else if (node.type==NodeType::WeaponMove) {
        s.field=Row(s.rect,0,26); s.field2=Row(s.rect,1,26);  // the weapon, its ability; then a note and the 派生点
        stepper(3);
    } else if (node.type==NodeType::Hook) {
        s.field=Row(s.rect,0,26); s.field2=Row(s.rect,1,26); s.field3=Row(s.rect,2,26);  // its mode, reach, speed; then a note and the 派生点
        stepper(4);
    } else if (node.type==NodeType::Teleport) {
        s.field=Row(s.rect,0,26); s.field2=Row(s.rect,1,26);  // distance, height; then a note and the 派生点
        stepper(3);
    } else if (node.type==NodeType::Group) {
        stepper(0);  // its 派生点 under the header; the nodes below; the size handle in the corner
        const int h=Px(16);
        s.resize={s.rect.right-h,s.rect.bottom-h,s.rect.right,s.rect.bottom};
    } else if (node.type==NodeType::BossEffect) {  // toggle / toggle2: its radius and hits fields
        s.field=Row(s.rect,0,26); s.field2=Row(s.rect,1,26); s.field3=Row(s.rect,2,26); s.field4=Row(s.rect,3,26); s.toggle=Row(s.rect,4,26); s.toggle2=Row(s.rect,5,26);
    } else if (node.type==NodeType::Air) {
        s.field=Row(s.rect,0,26); s.field2=Row(s.rect,1,26); s.field3=Row(s.rect,2,26); s.field4=Row(s.rect,3,26);  // then a note
    } else if (node.type==NodeType::AirDefault) {
        s.toggle=Row(s.rect,0); s.field=Row(s.rect,1); s.field2=Row(s.rect,2); s.field3=Row(s.rect,3); s.field4=Row(s.rect,4);  // then a note
    } else if (node.type==NodeType::Grab||node.type==NodeType::Look||node.type==NodeType::ForceHit) {
        s.field=Row(s.rect,0,26);
        if (LaunchShown(node)) s.field2=Row(s.rect,1,26);  // 击飞's height
        if (node.type==NodeType::Grab) s.toggle=Row(s.rect,2);  // 不变身
    } else if (node.type==NodeType::Trigger) {
        s.field=Row(s.rect,0,26);
        stepper(1);
        if (node.trigger!=static_cast<int>(Trigger::NpcKey)) s.toggle=Row(s.rect,2);
        if (ExecutionTrigger(node)) s.field2=Row(s.rect,3,26);  // whose executions: any weapon or one
        if (KeysTrigger(node)) { s.field2=Row(s.rect,3,26); s.field3=Row(s.rect,4,26); }  // its keys; in order or together
        if (RawTrigger(node)) s.toggle2=Row(s.rect,5);  // 吞掉
        if (KeysTrigger(node)) s.field4=Row(s.rect,RawTrigger(node)?6:5,26);  // 继续派生
        // 按住: the last row, a stepper as the reset time's.
        const int hold_row=node.trigger==static_cast<int>(Trigger::NpcKey)?2:KeysTrigger(node)?(RawTrigger(node)?7:6):ExecutionTrigger(node)?4:3;
        const RECT line=Row(s.rect,hold_row); const int b=line.bottom-line.top;
        const int hold_w=Px(TriggerHoldUnits(dc));
        s.hminus={line.right-hold_w,line.top,line.right-hold_w+b,line.bottom}; s.hplus={line.right-b,line.top,line.right,line.bottom};
        s.hvalue={s.hminus.right+Px(4),line.top,s.hplus.left-Px(4),line.bottom}; s.has_hold=true;
        s.toggle3=Row(s.rect,hold_row+1);  // 整体不变身
    } else if (node.type==NodeType::WeaponActions) {
        // The weapon, the reset time, a note, then a row per action (its field, x, its own output) and "+ 添加动作".
        s.field=Row(s.rect,0,26);
        stepper(1);
        int at=3;
        for (size_t k=0;k<node.routes.size();++k) {
            RouteShape route; const RECT line=Row(s.rect,at++); const int b=line.bottom-line.top,right=s.rect.right-Px(30);
            route.remove={right-b,line.top,right,line.bottom};
            route.hplus={route.remove.left-Px(8)-b,line.top,route.remove.left-Px(8),line.bottom};
            route.hvalue={route.hplus.left-Px(4)-Px(RouteHoldUnits(dc)),line.top,route.hplus.left-Px(4),line.bottom};
            route.hminus={route.hvalue.left-Px(4)-b,line.top,route.hvalue.left-Px(4),line.bottom};
            route.field={line.left,line.top,route.hminus.left-Px(8),line.bottom};
            route.out={s.rect.right-Px(13),(line.top+line.bottom)/2};
            if(NpcCombo::KeysRoute(node.routes[k])){const RECT keys=Row(s.rect,at++);const int split=keys.left+Px(16)+(right-keys.left-Px(16))*11/20;
                route.has_keys=true;route.keys={keys.left+Px(16),keys.top,split-Px(3),keys.bottom};route.mode={split+Px(3),keys.top,right,keys.bottom};}
            s.routes.push_back(route);
        }
        const RECT add=Row(s.rect,at);
        s.add={add.left,add.top,add.left+Px(132),add.bottom};
        s.toggle3=Row(s.rect,at+1);  // 整体不变身
    } else if (node.type==NodeType::Branch) {
        // Rows: the note, one per route (button field, pause stepper, x, its own output; a 自定义组合键 / 自定义按键
        // row's keys and mode on a line under it), then "+ 添加按键". The 按住 and pause boxes fit their widest values.
        const int hold_v=RouteHoldUnits(dc),pause_v=RoutePauseUnits(dc);
        int at=1;
        for (size_t k=0;k<node.routes.size();++k) {
            RouteShape route; const RECT line=Row(s.rect,at++); const int b=line.bottom-line.top,right=s.rect.right-Px(30);
            route.field={line.left,line.top,line.left+Px(132),line.bottom};
            route.remove={right-b,line.top,right,line.bottom};
            route.plus={route.remove.left-Px(8)-b,line.top,route.remove.left-Px(8),line.bottom};
            route.value={route.plus.left-Px(4)-Px(pause_v),line.top,route.plus.left-Px(4),line.bottom};
            route.minus={route.value.left-Px(4)-b,line.top,route.value.left-Px(4),line.bottom};
            route.hplus={route.minus.left-Px(8)-b,line.top,route.minus.left-Px(8),line.bottom};
            route.hvalue={route.hplus.left-Px(4)-Px(hold_v),line.top,route.hplus.left-Px(4),line.bottom};
            route.hminus={route.hvalue.left-Px(4)-b,line.top,route.hvalue.left-Px(4),line.bottom};
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
// Clipboard stays within the panel process and can be pasted into another table.
float ToolsBottom();
void Refresh(bool force);
void CopySelection(){
    Refresh(false);
    if(!HasSkin())return;
    const auto selection=Selection();int skipped=0;
    for(int index:selection)if(At(index)&&!NpcCombo::ClipboardNode(*At(index)))++skipped;
    auto copied=NpcCombo::CopyNodes(config_,selection);
    if(copied.empty()){DebugHub::Notify(Tr(L"请选择可复制的节点；按键、预输入和空中释放是唯一节点，不能复制。").c_str());return;}
    node_clipboard_=std::move(copied);paste_count_=0;
    wchar_t notice[192]{};_snwprintf_s(notice,_countof(notice),_TRUNCATE,Tr(L"已复制 %zu 个节点（跳过 %d 个唯一节点）；Ctrl+V 粘贴，外部连线不复制。").c_str(),node_clipboard_.size(),skipped);
    DebugHub::Notify(notice);Invalidate();
}
void PasteSelection(){
    Refresh(false);
    if(!HasSkin()||node_clipboard_.empty())return;
    const int top=canvas_.top+static_cast<int>(std::lround(ToolsBottom()));
    POINT anchor=cursor_;if(!PtInRect(&canvas_,anchor)||anchor.y<top)anchor={(canvas_.left+canvas_.right)/2,top+M().S(48)};
    int min_x=NpcCombo::kCanvasLimit,min_y=NpcCombo::kCanvasLimit;for(const auto& node:node_clipboard_){min_x=(std::min)(min_x,node.x);min_y=(std::min)(min_y,node.y);}
    const int offset=32*(std::min)(paste_count_+1,1000);
    const int dx=NpcCombo::ClampPosition(static_cast<int>(ToCanvasX(anchor.x)))-min_x+offset,dy=NpcCombo::ClampPosition(static_cast<int>(ToCanvasY(anchor.y)))-min_y+offset;
    auto added=NpcCombo::PasteNodes(config_,node_clipboard_,dx,dy);
    if(added.empty()){DebugHub::Notify(Tr(L"无法粘贴：节点数量已到保护上限。").c_str());return;}
    ++paste_count_;picked_=std::move(added);selected_=picked_.back();Save();Layout();Invalidate();
    wchar_t notice[128]{};_snwprintf_s(notice,_countof(notice),_TRUNCATE,Tr(L"已粘贴 %zu 个节点，内部连线和参数已保留。").c_str(),picked_.size());DebugHub::Notify(notice);
}
bool ClipboardKey(WPARAM key,bool control){
    if(!control||(key!='C'&&key!='V')||search_.open||edit_.node>=0||record_.node>=0||drag_.mode!=Drag::None)return false;
    if(key=='C')CopySelection();else PasteSelection();return true;
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
    wchar_t text[160]{}; _snwprintf_s(text,_countof(text),_TRUNCATE,Tr(L"节点已有 %zu 个，到了防止文件损坏的保护上限。").c_str(),NpcCombo::kMaxNodes);
    DebugHub::Notify(text); return false;
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
    CloseCurve("nodes removed");
    std::vector<int> gone=Selection();
    if (gone.size()>1) {
        // The whole selection, last index first (the 按键 and 预输入 nodes stay).
        size_t kept=0;
        gone.erase(std::remove_if(gone.begin(),gone.end(),[&](int i){ const bool keep=IsKey(i)||IsSettingAt(i); kept+=keep?1:0; return keep; }),gone.end());
        std::sort(gone.rbegin(),gone.rend());
        for (const int i:gone) NpcCombo::Remove(config_,static_cast<size_t>(i));
        wchar_t text[160]{};
        _snwprintf_s(text,_countof(text),_TRUNCATE,Tr(kept?L"已删除 %zu 个节点（「按键」「预输入」「空中释放」节点保留）":L"已删除 %zu 个节点").c_str(),gone.size());
        DebugHub::Notify(text);
        picked_.clear(); Select(-1); Save(); Invalidate(); return;
    }
    if (!At(selected_)) return;
    if (IsKey(selected_)) { DebugHub::Notify(Tr(L"「按键」节点不能删除；断开它的连线即可停用。").c_str()); return; }
    if (IsPreInputAt(selected_)) { DebugHub::Notify(Tr(L"「预输入」节点不能删除；时间设为 0 即一直保留。").c_str()); return; }
    if (IsSettingAt(selected_)) { DebugHub::Notify(Tr(L"「空中释放」节点不能删除；不需要时把它关闭。").c_str()); return; }
    NpcCombo::Remove(config_,static_cast<size_t>(selected_)); picked_.clear(); Select(-1); Save(); Invalidate();
}
// 编辑曲线…: the node's curve in its own window (曲线调节 turns on with it). The window reads the node back every
// tick and writes it (saved when a drag or click is over); it closes when the node goes (removed, another chain or
// file loaded).
void OpenBones(int index){
    if(!At(index)||config_.chain[static_cast<size_t>(index)].type!=NodeType::BoneTransform)return;
    SpeedCurveWindow::Close("bone editor");const std::string profile=moves_.profile;
    BoneMotionPage::Host host;
    host.read=[index,profile](BoneMotion::Config& bones,std::wstring& title){
        if(moves_.profile!=profile||!At(index)||config_.chain[static_cast<size_t>(index)].type!=NodeType::BoneTransform)return false;
        bones=config_.chain[static_cast<size_t>(index)].bones;title=L"骨骼变换 · 节点 "+std::to_wstring(index+1);return true;};
    host.write=[index,profile](const BoneMotion::Config& bones){
        if(moves_.profile!=profile||!At(index)||config_.chain[static_cast<size_t>(index)].type!=NodeType::BoneTransform)return;
        config_.chain[static_cast<size_t>(index)].bones=BoneMotion::Clean(bones);Save();Layout();Invalidate();};
    BoneMotionPage::Open(window_,index,std::move(host));
}
void OpenCurve(int index) {
    BoneMotionPage::Close();
    if (!At(index)||config_.chain[static_cast<size_t>(index)].type!=NodeType::Speed) return;
    Node& node=config_.chain[static_cast<size_t>(index)];
    if (!node.curve||node.points.size()<2) { node.curve=true; if (node.points.size()<2) node.points=NpcCombo::DefaultCurve(node.speed); Save(); Layout(); }
    const std::string profile=moves_.profile;
    SpeedCurveWindow::Host host;
    host.read=[index,profile](std::vector<NpcCombo::CurvePoint>& points,bool& on,int& speed,std::wstring& title) {
        if (moves_.profile!=profile||!At(index)||config_.chain[static_cast<size_t>(index)].type!=NodeType::Speed) return false;
        const Node& n=config_.chain[static_cast<size_t>(index)];
        points=n.points; on=n.curve; speed=n.speed;
        const NpcCombo::Pointers from=NpcCombo::PointersTo(config_,index);
        wchar_t text[96]{};
        if (from.triggers) _snwprintf_s(text,_countof(text),_TRUNCATE,L"%s",Tr(L"整条链").c_str());
        else _snwprintf_s(text,_countof(text),_TRUNCATE,Tr(L"指向它的 %d 招").c_str(),from.nodes);
        title=text; return true;
    };
    host.write=[index,profile](const std::vector<NpcCombo::CurvePoint>& points,bool on,bool commit) {
        if (moves_.profile!=profile||!At(index)||config_.chain[static_cast<size_t>(index)].type!=NodeType::Speed) return;
        Node& n=config_.chain[static_cast<size_t>(index)];
        const bool relayout=n.curve!=on;
        n.points=NpcCombo::CleanCurve(points); n.curve=on;
        if (commit) Save();
        if (relayout) Layout();
        Invalidate();
    };
    SpeedCurveWindow::Open(window_,index,std::move(host));
}
// The curve window goes with its node (a removal shifts the indexes; another chain or file replaces them).
void CloseCurve(const char* why) { BoneMotionPage::Close(); if (SpeedCurveWindow::EditedNode()>=0) SpeedCurveWindow::Close(why); }
// combo curve: the first 加速 node's window opened as a click on it does (曲线调节 turns on with it), or a test curve
// (never saved) when the chain has none; where it sits. combo curve close puts that node's 曲线调节 and points back.
struct CurveTest { int node=-1; std::string profile; bool curve=false; std::vector<NpcCombo::CurvePoint> points; } curve_test_;
void TestCurve(bool close) {
    if (close) {
        SpeedCurveWindow::Close("test closed it");
        const CurveTest t=curve_test_; curve_test_={};
        if (t.node>=0&&moves_.profile==t.profile&&At(t.node)&&config_.chain[static_cast<size_t>(t.node)].type==NodeType::Speed) {
            Node& n=config_.chain[static_cast<size_t>(t.node)];
            if (n.curve!=t.curve||n.points.size()!=t.points.size()) { n.curve=t.curve; n.points=t.points; Save(); Layout(); Invalidate(); }
            Logf("COMBO_TEST curve closed; node %d put back (curve %d, %zu points)",t.node,t.curve?1:0,t.points.size());
        } else Logf("COMBO_TEST curve closed");
        return;
    }
    if (!window_||!IsWindowVisible(window_)) { Logf("COMBO_TEST curve refused: the 招式派生 page is not shown"); return; }
    int node=-1;
    for (size_t i=0;i<config_.chain.size();++i) if (config_.chain[i].type==NodeType::Speed) { node=static_cast<int>(i); break; }
    if (node>=0) {
        if (curve_test_.node!=node) { const Node& n=config_.chain[static_cast<size_t>(node)]; curve_test_={node,moves_.profile,n.curve,n.points}; }
        OpenCurve(node);
    }
    else {
        static std::vector<NpcCombo::CurvePoint> points; static bool on=true;
        if (points.empty()) points={{0,100},{40,250},{100,100}};
        SpeedCurveWindow::Host host;
        host.read=[](std::vector<NpcCombo::CurvePoint>& p,bool& o,int& speed,std::wstring& title) { p=points; o=on; speed=100; title=L"测试"; return true; };
        host.write=[](const std::vector<NpcCombo::CurvePoint>& p,bool o,bool) { points=NpcCombo::CleanCurve(p); on=o; };
        SpeedCurveWindow::Open(window_,100000,std::move(host));
    }
    const HWND curve=SpeedCurveWindow::Window(),parent=curve?GetParent(curve):nullptr,game=UiHost::Game();
    RECT r{},page{}; if (curve) GetWindowRect(curve,&r); GetWindowRect(window_,&page);
    const LONG_PTR style=curve?GetWindowLongPtrW(curve,GWL_STYLE):0;
    Logf("COMBO_TEST curve node=%d window=%p child=%d parent=%p parentHoldsPage=%d parentOnGame=%d visible=%d rect=%ld,%ld %ldx%ld page=%ld,%ld %ldx%ld",
         node,static_cast<void*>(curve),(style&WS_CHILD)?1:0,static_cast<void*>(parent),parent&&IsChild(parent,window_)?1:0,
         parent&&GetParent(parent)==game?1:0,curve&&IsWindowVisible(curve)?1:0,r.left,r.top,r.right-r.left,r.bottom-r.top,
         page.left,page.top,page.right-page.left,page.bottom-page.top);
}
// One step up (dir 1) or down (-1) of a node's value: an interval, a trigger's reset time, a 加速's speed, a move's
// 派生点, or the pause of a branch's route `route`.
void Nudge(int index,int dir,int route=-1) {
    if (!At(index)||!dir) return;
    Node& node=config_.chain[static_cast<size_t>(index)];
    dir=dir>0?1:-1;
    if (node.type==NodeType::Interval) node.ms=NpcCombo::ClampInterval(node.ms+dir*NpcCombo::kIntervalStep);
    else if (node.type==NodeType::PreInput) node.ms=NpcCombo::ClampPreInput(node.ms+dir*NpcCombo::kPreInputStep);
    else if (node.type==NodeType::KeepChain) node.ms=NpcCombo::ClampKeepChain(node.ms+dir*NpcCombo::kKeepChainStep);
    else if (NpcCombo::StartsChain(node)) node.ms=NpcCombo::ClampReset(node.ms+dir*NpcCombo::kResetStep);
    else if (node.type==NodeType::Speed) node.speed=NpcCombo::ClampSpeed(node.speed+dir*NpcCombo::kSpeedStep);
    else if (node.type==NodeType::Damage) node.speed=NpcCombo::ClampDamage(node.speed+dir*NpcCombo::kDamageStep);
    else if (node.type==NodeType::Trim) {
        // 开始于: never at or past a set end, nor past the move's length once it is known.
        int from=NpcCombo::ClampTrim(node.trim_from+dir*NpcCombo::kTrimStep);
        if (node.trim_to>node.trim_from&&from>=node.trim_to) from=(std::max)(0,node.trim_to-NpcCombo::kTrimStep);
        const int clip=static_cast<int>(std::lround(TrimClip(static_cast<size_t>(index))*1000));
        if (clip>0) from=(std::min)(from,(std::max)(0,clip-NpcCombo::kTrimStep));
        node.trim_from=from;
    }
    else if (node.type==NodeType::Skill||node.type==NodeType::AutoDodge||node.type==NodeType::BossSkill||node.type==NodeType::WeaponMove||node.type==NodeType::Hook||node.type==NodeType::Teleport||
             node.type==NodeType::Group)
        node.ms=NpcCombo::ClampCancel(node.ms+dir*NpcCombo::kCancelStep);
    else if (node.type==NodeType::Branch&&route>=0&&static_cast<size_t>(route)<node.routes.size())
        node.routes[static_cast<size_t>(route)].pause_ms=NpcCombo::ClampPause(node.routes[static_cast<size_t>(route)].pause_ms+dir*NpcCombo::kPauseStep);
    else return;
    Save(); Invalidate();
}
// A 按住 of node `index`: row -1 a trigger's own, else that branch row's (null: none there).
int* HoldOf(int index,int row) {
    if (!At(index)) return nullptr;
    Node& node=config_.chain[static_cast<size_t>(index)];
    if (row<0) return node.type==NodeType::Trigger?&node.hold_ms:node.type==NodeType::Trim?&node.trim_to:nullptr;  // a 裁剪's 结束于
    return NpcCombo::HasRows(node)&&static_cast<size_t>(row)<node.routes.size()?&node.routes[static_cast<size_t>(row)].hold_ms:nullptr;
}
void NudgeHold(int index,int dir,int row) {
    int* hold=HoldOf(index,row); if (!hold||!dir) return;
    if (config_.chain[static_cast<size_t>(index)].type==NodeType::Trim) {
        // 结束于: 放完 (0) is the move's end. A step down from it ends a step before the move's length (a second after
        // the start while the length is not known); a step up to the length is 放完 again.
        const int from=NpcCombo::ClampTrim(config_.chain[static_cast<size_t>(index)].trim_from);
        const int clip=static_cast<int>(std::lround(TrimClip(static_cast<size_t>(index))*1000));
        int to=0;
        if (*hold<=from) {
            if (dir>0&&*hold==0) return;  // already 放完
            to=dir>0?from+NpcCombo::kTrimStep:clip>from+NpcCombo::kTrimStep?clip-NpcCombo::kTrimStep:from+1000;
        } else {
            to=*hold+(dir>0?1:-1)*NpcCombo::kTrimStep;
            if (clip>0&&to>=clip) to=0; else if (to<=from) to=from+NpcCombo::kTrimStep;
        }
        *hold=NpcCombo::ClampTrim(to);
    } else *hold=NpcCombo::ClampHold(*hold+(dir>0?1:-1)*NpcCombo::kHoldStep);
    Save(); Invalidate();
}
// A value typed in: a 加速's multiplier (×0.50–×10.00) or a move's 派生点 in seconds (0–10; 0: when it ends). A
// click on the value starts it (the text selected), Enter / a click elsewhere keeps it (clamped), Esc drops it.
bool Typed(const Node& node) {
    return node.type==NodeType::Speed||node.type==NodeType::Damage||node.type==NodeType::Skill||node.type==NodeType::AutoDodge||node.type==NodeType::BossSkill||node.type==NodeType::WeaponMove||
           node.type==NodeType::Hook||node.type==NodeType::Teleport||node.type==NodeType::Group||node.type==NodeType::PreInput||node.type==NodeType::KeepChain||
           node.type==NodeType::Trim;
}
void BeginEdit(int index) {
    if (!At(index)||!Typed(config_.chain[static_cast<size_t>(index)])) return;
    const Node& node=config_.chain[static_cast<size_t>(index)];
    wchar_t text[16]{};
    if (node.type==NodeType::Damage) _snwprintf_s(text,_countof(text),_TRUNCATE,L"%d",NpcCombo::ClampDamage(node.speed));
    else if (node.type==NodeType::PreInput) _snwprintf_s(text,_countof(text),_TRUNCATE,L"%.2f",NpcCombo::ClampPreInput(node.ms)/1000.0);
    else if (node.type==NodeType::KeepChain) _snwprintf_s(text,_countof(text),_TRUNCATE,L"%.2f",NpcCombo::ClampKeepChain(node.ms)/1000.0);
    else if (node.type==NodeType::Trim) _snwprintf_s(text,_countof(text),_TRUNCATE,L"%.2f",NpcCombo::ClampTrim(node.trim_from)/1000.0);
    else _snwprintf_s(text,_countof(text),_TRUNCATE,L"%.2f",node.type==NodeType::Speed?NpcCombo::ClampSpeed(node.speed)/100.0:NpcCombo::ClampCancel(node.ms)/1000.0);
    edit_={index,text,true}; SetFocus(window_); Invalidate();
}
// A 按住 typed in seconds (0-5; 0: any press).
void BeginHoldEdit(int index,int row) {
    const int* hold=HoldOf(index,row); if (!hold) return;
    wchar_t text[16]{};
    if (config_.chain[static_cast<size_t>(index)].type==NodeType::Trim) _snwprintf_s(text,_countof(text),_TRUNCATE,L"%.2f",NpcCombo::ClampTrim(*hold)/1000.0);  // 结束于
    else _snwprintf_s(text,_countof(text),_TRUNCATE,L"%.1f",NpcCombo::ClampHold(*hold)/1000.0);
    edit_={index,text,true,row}; SetFocus(window_); Invalidate();
}
void CancelEdit() { if (edit_.node>=0) { edit_={}; Invalidate(); } }
void CommitEdit() {
    if (edit_.node<0) return;
    const Edit done=edit_; edit_={};
    if (done.hold>-2) {
        int* hold=HoldOf(done.node,done.hold);
        wchar_t* end=nullptr; const double value=done.text.empty()?-1:std::wcstod(done.text.c_str(),&end);
        if (hold&&end&&end!=done.text.c_str()&&value>=0) {
            const int ms=static_cast<int>(std::lround(value*1000));
            const bool trim=config_.chain[static_cast<size_t>(done.node)].type==NodeType::Trim;  // a 裁剪's 结束于
            *hold=trim?NpcCombo::ClampTrim(ms):NpcCombo::ClampHold(ms);
            if (*hold!=ms) DebugHub::Notify(Tr(trim?L"裁剪时间在 0 到 60 秒之间，已调整。":L"按住时间在 0 到 5 秒之间，已调整。").c_str());
            Save();
        }
        Invalidate(); return;
    }
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
        } else if (end&&end!=done.text.c_str()&&node->type==NodeType::KeepChain&&value>=0) {
            const int ms=static_cast<int>(std::lround(value*1000));
            node->ms=NpcCombo::ClampKeepChain(ms);
            if (node->ms!=ms) DebugHub::Notify(Tr(L"保持派生在 0.5 到 30 秒之间，已调整。").c_str());
            Save();
        } else if (end&&end!=done.text.c_str()&&node->type==NodeType::Trim&&value>=0) {
            const int ms=static_cast<int>(std::lround(value*1000));
            node->trim_from=NpcCombo::ClampTrim(ms);
            if (node->trim_from!=ms) DebugHub::Notify(Tr(L"裁剪时间在 0 到 60 秒之间，已调整。").c_str());
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
    if (node.routes.size()>=NpcCombo::kMaxRoutes) {
        DebugHub::Notify(Tr(node.air_set?L"空中派生最多 6 行。":actions?L"武器派生修改最多 6 行。":L"按键分支最多 6 行。").c_str()); return; }
    NpcCombo::Route route;
    if (node.air_set) {
        for (const Trigger t:{Trigger::Light,Trigger::Heavy,Trigger::Shachi,Trigger::CombatArt,Trigger::NpcKey,Trigger::Special,Trigger::Jump,Trigger::AnyDodge,Trigger::Stance})
            if (std::none_of(node.routes.begin(),node.routes.end(),[t](const NpcCombo::Route& r){return r.button==static_cast<int>(t);})) { route.button=static_cast<int>(t); break; }
        node.routes.push_back(route); Save(); Invalidate(); return;
    }
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
    if (node.routes.size()<=1) {
        DebugHub::Notify(Tr(node.air_set?L"空中派生至少保留一行；不要的话删掉整个节点。":WeaponActionsNode(node)?L"武器派生修改至少保留一行；不要的话删掉整个节点。":
                            L"按键分支至少保留一行；不要的话删掉整个节点。").c_str()); return; }
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
    for (const Button b:kCanvasTools)
        bottom=(std::max)(bottom,buttons_[static_cast<int>(b)].bottom);
    return static_cast<float>(bottom-canvas_.top+M().S(14));
}
// everything: every node in view (largest zoom up to 1:1 that fits, centred; the top first when even the smallest
// zoom is too tall). Otherwise the opening view: the width fitted at 75 % or more, from the top-left, under the tool
// bar.
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
        zoom_index_=kZoomOpen;
        for (int i=kZoomOne;i>=kZoomOpen;--i) if ((all.right-all.left)*kZooms[i]*Dpi()<=w) { zoom_index_=i; break; }
        pan_x_=M().S(20)-all.left*Scale(); pan_y_=ToolsBottom()-all.top*Scale();
        Invalidate(); return;
    }
    zoom_index_=0;
    for (int i=kZoomOne;i>=0;--i) if ((all.right-all.left)*kZooms[i]*Dpi()<=w&&(all.bottom-all.top)*kZooms[i]*Dpi()<=h) { zoom_index_=i; break; }
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
    // Depth first with a stack of its own: a straight run goes on in a loop; a branch's first route goes on along the
    // row, each other one (once those before it are laid out) on a new row from the branch's right edge.
    struct Step { int node; int indent; bool row; };
    std::vector<Step> steps;
    const auto walk=[&](int start,int start_indent) {
        steps.clear(); steps.push_back({start,start_indent,false});
        while (!steps.empty()) {
            const Step step=steps.back(); steps.pop_back();
            if (step.node<0||step.node>=count||placed[static_cast<size_t>(step.node)]) continue;
            if (step.row) new_row(step.indent);
            for (int i=step.node;i>=0&&i<count&&!placed[static_cast<size_t>(i)];) {
                const auto mods=config_.chain[static_cast<size_t>(i)].mods;
                for (const int m:mods) if (m>=0&&m<count&&!placed[static_cast<size_t>(m)]&&NpcCombo::IsModifier(config_.chain[static_cast<size_t>(m)])) place(static_cast<size_t>(m),step.indent);
                place(static_cast<size_t>(i),step.indent);
                const Node& node=config_.chain[static_cast<size_t>(i)];
                if (!NpcCombo::HasRows(node)) { i=node.next; continue; }
                const int right=node.x+static_cast<int>(NodeSize(dc,static_cast<size_t>(i)).cx)+gap;
                for (size_t r=node.routes.size();r-->0;)  // last first: the first route comes off the stack first
                    steps.push_back({node.routes[r].next,r>0?(right+300<width?right:step.indent):step.indent,r>0});
                break;
            }
        }
    };
    // 预输入 (the chain's setting) first, on a row of its own at the top-left.
    {   // 空中释放 beside it
        const int pre=NpcCombo::PreInputNode(config_),air=NpcCombo::AirDefaultNode(config_); bool any=false;
        if (pre>=0&&!placed[static_cast<size_t>(pre)]) { place(static_cast<size_t>(pre),24); any=true; }
        if (air>=0&&!placed[static_cast<size_t>(air)]) { place(static_cast<size_t>(air),24); any=true; }
        if (any) { x=24; y=bottom+row_gap; row_h=0; }
    }
    std::vector<int> triggers;
    const int key=NpcCombo::KeyTrigger(config_); if (key>=0) triggers.push_back(key);
    for (int i=0;i<count;++i) if (NpcCombo::StartsChain(config_.chain[static_cast<size_t>(i)])&&i!=key) triggers.push_back(i);
    bool first=true;
    for (const int t:triggers) {
        if (!first) { x=24; y=bottom+row_gap*2; row_h=0; }
        first=false;
        walk(t,24);
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
        add({ItemKind::BoneTransform,Tr(L"骨骼变换"),flow});
        add({ItemKind::Speed,Tr(L"加速"),flow});
        add({ItemKind::Armor,Tr(L"霸体"),flow});
        add({ItemKind::Invincible,Tr(L"无敌"),flow});
        add({ItemKind::IgnoreGuard,Tr(L"无视防御"),flow});
        add({ItemKind::Interrupt,Tr(L"允许中断"),flow});
        add({ItemKind::KeepChain,Tr(L"保持派生"),flow});
        add({ItemKind::Damage,Tr(L"伤害倍率"),flow});
        if (At(search_.mod_from)&&NpcCombo::StartsChain(config_.chain[static_cast<size_t>(search_.mod_from)])) add({ItemKind::Weapon,Tr(L"武器属性"),flow});
        if (At(search_.mod_from)) {
            const NodeType from=config_.chain[static_cast<size_t>(search_.mod_from)].type;
            if (NpcCombo::StartsChain(config_.chain[static_cast<size_t>(search_.mod_from)])||from==NodeType::BossSkill||from==NodeType::Group) add({ItemKind::KeepLook,Tr(L"保留变身"),flow});
            if (NpcCombo::TakesTrim(config_.chain[static_cast<size_t>(search_.mod_from)])) add({ItemKind::Trim,Tr(L"裁剪"),flow});  // a part of its own montage
        }
        add({ItemKind::ForceHit,Tr(L"强制受击"),flow});
        if (At(search_.mod_from)) {
            const Node& from=config_.chain[static_cast<size_t>(search_.mod_from)];
            if (NpcCombo::StartsChain(from)||NpcCombo::TakesAir(from)||from.type==NodeType::Group) add({ItemKind::Air,Tr(L"滞空"),flow});
            if (NpcCombo::ModifierFits([&]{ std::vector<Node> two{from,NpcCombo::ModifierNode(NodeType::BossEffect,0,0)}; return two; }(),0,1)) add({ItemKind::BossEffect,Tr(L"BOSS 特效"),flow});
        }
        return;
    }
    if (search_.mode==1) {
        // The .combo files in combos\ (the active one first).
        for (const std::wstring& name:files_.names) if (name==files_.active) add({ItemKind::File,name,Tr(L"当前文件")});
        for (const std::wstring& name:files_.names) if (name!=files_.active) add({ItemKind::File,name,Tr(L"combos 文件夹")});
        return;
    }
    if (search_.mode==6) {
        // 招式来源: this table's own look, or a boss whose moves the list then shows (added as BOSS 技能).
        Item own{ItemKind::MoveSource,Tr(L"本表外观")+L"（"+Tr(moves_.name.c_str())+L"）",Tr(L"招式来源")}; add(own);
        for (const auto& [id,name]:BossLooks()) {
            if (id==moves_.profile) continue;
            Item item{ItemKind::MoveSource,Tr(name.c_str()),Tr(L"其他 Boss（加为 BOSS 技能）")}; item.ability=id; add(item);
        }
        return;
    }
    if (search_.mode==4) {
        // 派生页的技能伤害: follow the NPC 技能 page, or a multiplier of the boss's own damage.
        const std::wstring group=Tr(L"派生链的 BOSS 技能伤害");
        Item follow{ItemKind::ChainSkill,Tr(L"跟随 NPC 技能页"),group}; add(follow);
        for (const float v:{.5f,1.f,1.5f,2.f,3.f,5.f,8.f,10.f}) { Item item{ItemKind::ChainSkill,ChainSkillText(v),group}; item.value=v; add(item); }
        return;
    }
    if (search_.mode==5) {
        // 派生页的投技伤害: follow the 投技 page, the grab's own damage, a multiplier, or at least a share of max health.
        const std::wstring group=Tr(L"派生链的投技伤害");
        Item follow{ItemKind::ChainGrab,Tr(L"跟随投技页"),group}; follow.trigger=NpcCombo::kGrabFollow; add(follow);
        Item native{ItemKind::ChainGrab,Tr(L"原生伤害"),group}; native.trigger=0; add(native);
        for (const float v:{2.f,4.5f,10.f,15.f,20.f,30.f}) { Item item{ItemKind::ChainGrab,ChainGrabText({1,v}),group}; item.trigger=1; item.value=v; add(item); }
        for (const float v:{10.f,20.f,30.f,50.f,100.f}) { Item item{ItemKind::ChainGrab,ChainGrabText({2,v}),group}; item.trigger=2; item.value=v; add(item); }
        return;
    }
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
    if (search_.pick_node>=0&&At(search_.pick_node)&&NpcCombo::PicksBossMove(config_.chain[static_cast<size_t>(search_.pick_node)])) {
        const Node& node=config_.chain[static_cast<size_t>(search_.pick_node)];
        if (search_.pick_route==-2) {
            // That look's moves.
            NpcRetarget::SkinMoves look; const int32_t row=BossLookRow(node.boss);
            if (row>=0&&NpcRetarget::GetSkinMoves(row,look))
                for (const std::string& ability:look.skills) { Item item{ItemKind::BossMove,MoveText(ability),Tr(L"招式 · ")+Tr(NpcCombo::KindLabel(NpcCombo::Classify(ability)))}; item.ability=ability; add(item); }
        } else for (const auto& [id,name]:BossLooks()) { Item item{ItemKind::BossLook,name,Tr(L"外观")}; item.ability=id; add(item); }
        return;
    }
    if (search_.pick_node>=0&&At(search_.pick_node)&&search_.pick_part==0&&WeaponActionsNode(config_.chain[static_cast<size_t>(search_.pick_node)])) {
        // A 武器派生修改's weapon (route -1), or a row's action: the hero's actions (处决 takes the quick ones too unless a
        // 简易处决 row is there).
        if (search_.pick_route<0) {
            for (const auto& [id,shaqi]:ListedWeapons()) { Item item{ItemKind::WeaponPick,WeaponName(id),Tr(shaqi?L"影之武装":L"武器")}; item.ability=id; add(item); }
            return;
        }
        const bool air=config_.chain[static_cast<size_t>(search_.pick_node)].air_set;
        const std::wstring group=Tr(air?L"在空中按哪个键":L"改哪个动作");
        if (air) { Item item{ItemKind::Button,ButtonLabel(static_cast<int>(Trigger::NpcKey)),group}; item.trigger=static_cast<int>(Trigger::NpcKey); add(item); }
        for (const Trigger t:NpcCombo::kTriggerOrder) if (!NpcCombo::OwnKeysTrigger(static_cast<int>(t))&&(air?NpcCombo::ValidAirAction(static_cast<int>(t)):NpcCombo::ValidWeaponAction(static_cast<int>(t)))) {
            Item item{ItemKind::Button,ButtonLabel(static_cast<int>(t)),group}; item.trigger=static_cast<int>(t); add(item);
        }
        for(const Trigger t:{Trigger::Combo,Trigger::Keys}){Item item{ItemKind::Button,ButtonLabel(static_cast<int>(t)),Tr(L"特殊组合输入")};item.trigger=static_cast<int>(t);add(item);}
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
    if (search_.pick_node>=0&&At(search_.pick_node)&&config_.chain[static_cast<size_t>(search_.pick_node)].type==NodeType::BossEffect) {
        // BOSS 特效: route -1 its source folder, -2 its effect (that folder's), -3 place, -4 damage, -5 radius, -6 hits.
        const Node& node=config_.chain[static_cast<size_t>(search_.pick_node)];
        const int route=search_.pick_route;
        if (route==-1) {
            for (const std::string& folder:NpcBossEffects::Folders()) {
                const std::wstring wide(folder.begin(),folder.end()),name=FxFolderName(folder);
                const bool own=folder=="Common"||folder=="Weapon"||folder=="ShachiWeapon"||folder=="Hun";
                Item item{ItemKind::FxFolder,name,Tr(own?L"通用与主角":L"BOSS 与敌人")}; item.ability=folder; item.keys=Lower(name+L" "+wide); add(item);
            }
            std::stable_sort(search_.items.begin(),search_.items.end(),[](const Item& a,const Item& b){ return a.group<b.group; });  // BOSS 与敌人 first (B before 通)
            return;
        }
        if (route==-2) {
            for (const NpcBossEffect& e:kNpcBossEffects) {
                if (!node.boss.empty()&&node.boss!=e.folder) continue;
                const std::string short_name=NpcBossEffects::ShortName(e);
                Item item{ItemKind::FxPick,std::wstring(short_name.begin(),short_name.end()),FxFolderName(e.folder)};
                item.ability=e.name; item.keys=Lower(std::wstring(item.ability.begin(),item.ability.end())); add(item);
            }
            return;
        }
        if (route==-3) {
            for (int p=0;p<static_cast<int>(NpcCombo::EffectPlace::Count);++p) { Item item{ItemKind::FxPlace,FxPlaceText(p),Tr(L"特效位置")}; item.trigger=p; add(item); }
            return;
        }
        const ItemKind kind=route==-4?ItemKind::FxDamage:route==-5?ItemKind::FxRadius:ItemKind::FxHits;
        Node probe=node;
        const auto text=[&](int v){ if (kind==ItemKind::FxDamage) probe.fx_damage=v; else if (kind==ItemKind::FxRadius) probe.fx_radius=v; else probe.fx_hits=v; return FxFieldText(probe,route); };
        wchar_t* end=nullptr; const double typed=search_.query.empty()?-1:std::wcstod(search_.query.c_str(),&end);
        if (!search_.query.empty()&&end&&!*end&&typed>=0) {  // percent, metres or hits
            const int value=static_cast<int>(std::lround(kind==ItemKind::FxRadius?typed*100:typed));
            Item item{kind,Tr(L"自定义：")+text(value),Tr(L"输入的数值")}; item.trigger=std::clamp(value,0,1000000); add(item);
            search_.items.back().keys=Lower(search_.query);
        }
        const std::vector<int> presets=kind==ItemKind::FxDamage?std::vector<int>{0,5,10,15,20,30,50,100}:
                                       kind==ItemKind::FxRadius?std::vector<int>{200,300,400,600,800,1000,1500,2000}:std::vector<int>{1,2,3,4,5,6,8,10};
        const std::wstring group=Tr(kind==ItemKind::FxDamage?L"每段伤害（最大生命的百分比）":kind==ItemKind::FxRadius?L"伤害范围":L"伤害段数");
        for (const int v:presets) { Item item{kind,text(v),group}; item.trigger=v; add(item); }
        return;
    }
    if (search_.pick_node>=0&&At(search_.pick_node)&&(AirNode(config_.chain[static_cast<size_t>(search_.pick_node)])||
                                                      (LaunchShown(config_.chain[static_cast<size_t>(search_.pick_node)])&&search_.pick_route==-2))) {
        // A 滞空 / 空中释放 value (route -1 .. -4) or 击飞's height: a few values, or the number typed (lifts and heights
        // in metres, hangs in percent).
        const Node& node=config_.chain[static_cast<size_t>(search_.pick_node)];
        const bool launch=node.type==NodeType::ForceHit,hang=!launch&&AirHangRoute(search_.pick_route);
        const ItemKind kind=launch?ItemKind::Launch:hang?ItemKind::AirHang:ItemKind::AirLift;
        const auto text=[&](int v){ return launch?LaunchText(v):AirText(search_.pick_route,v); };
        const std::wstring group=Tr(launch?L"击飞高度":hang?L"滞空强度（重力减少）":L"升高");
        wchar_t* end=nullptr; const double typed=search_.query.empty()?-1:std::wcstod(search_.query.c_str(),&end);
        if (!search_.query.empty()&&end&&!*end&&typed>=0) {
            const int value=static_cast<int>(std::lround(hang?typed:typed*100));
            Item item{kind,Tr(L"自定义：")+text(value),Tr(L"输入的数值")}; item.trigger=std::clamp(value,0,1000000); add(item);
            search_.items.back().keys=Lower(search_.query);
        }
        const std::vector<int> presets=launch?std::vector<int>(std::begin(kLaunchPresets),std::end(kLaunchPresets)):
                                       hang?std::vector<int>(std::begin(kAirHangPresets),std::end(kAirHangPresets)):std::vector<int>(std::begin(kAirLiftPresets),std::end(kAirLiftPresets));
        for (const int v:presets) { Item item{kind,text(v),group}; item.trigger=v; add(item); }
        return;
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
            Item record{ItemKind::ThenKey,Tr(L"录制一个键：键盘、鼠标五键、手柄或左摇杆（回车确认）"),Tr(L"录制")}; record.trigger=-4; add(record);
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
                Item record{ItemKind::RawKey,Tr(L"录制按键：键盘、鼠标五键、手柄或左摇杆（回车确认）"),Tr(L"录制")}; record.trigger=-4; add(record);
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
        const bool weapon_keys=spec.row&&config_.chain[static_cast<size_t>(search_.pick_node)].type==NodeType::WeaponActions;
        add({ItemKind::ComboMode,Tr(weapon_keys?L"一起按（可先按住格挡，再按另一键）":L"一起按（150 毫秒内同时按下）"),Tr(L"一起按")});
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
    if (search_.pick_node>=0&&At(search_.pick_node)&&config_.chain[static_cast<size_t>(search_.pick_node)].type==NodeType::Hook) {
        // A 钩子's mode (route -1: the target pulled over, or the hero pulled to it), its reach (-2, metres) or its
        // speed (-3, a multiplier): a few values, or the number typed.
        if (search_.pick_route==-2||search_.pick_route==-3) {
            const bool speed=search_.pick_route==-3;
            const std::wstring group=Tr(speed?L"钩子速度":L"钩子最远距离");
            wchar_t* end=nullptr; const double typed=search_.query.empty()?-1:std::wcstod(search_.query.c_str(),&end);
            if (!search_.query.empty()&&end&&!*end&&typed>0) {
                // Metres for the reach; a multiplier for the speed (a number above 5 is read as percent).
                const int value=speed?static_cast<int>(std::lround(typed<=5?typed*100:typed)):static_cast<int>(std::lround(typed*100));
                Item item{speed?ItemKind::HookSpeed:ItemKind::HookDist,Tr(L"自定义：")+(speed?HookSpeedText(value):HookDistText(value)),Tr(L"输入的数值")};
                item.trigger=std::clamp(value,0,1000000); add(item);
                search_.items.back().keys=Lower(search_.query);
            }
            for (const int v:speed?std::vector<int>(std::begin(kHookSpeedPresets),std::end(kHookSpeedPresets)):std::vector<int>(std::begin(kHookDistPresets),std::end(kHookDistPresets))) {
                Item item{speed?ItemKind::HookSpeed:ItemKind::HookDist,speed?HookSpeedText(v):HookDistText(v),group}; item.trigger=v; add(item);
            }
            return;
        }
        const std::wstring group=Tr(L"钩子");
        for (const int mode:{2,0,1}) { Item item{ItemKind::HookMode,HookModeText(mode),group}; item.trigger=mode; add(item); }
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
    add({ItemKind::BoneTransform,Tr(L"骨骼变换"),flow});
        add({ItemKind::Speed,Tr(L"加速"),flow});
    add({ItemKind::Armor,Tr(L"霸体"),flow});
    add({ItemKind::Invincible,Tr(L"无敌"),flow});
    add({ItemKind::IgnoreGuard,Tr(L"无视防御"),flow});
    add({ItemKind::KeepLook,Tr(L"保留变身"),flow});
    add({ItemKind::ForceHit,Tr(L"强制受击"),flow});
    add({ItemKind::Air,Tr(L"滞空"),flow});
    add({ItemKind::BossEffect,Tr(L"BOSS 特效"),flow});
    add({ItemKind::Interrupt,Tr(L"允许中断"),flow});
    add({ItemKind::KeepChain,Tr(L"保持派生"),flow});
    add({ItemKind::Weapon,Tr(L"武器属性"),flow});
    add({ItemKind::Grab,Tr(L"投技"),flow});
    add({ItemKind::BossSkill,Tr(L"BOSS 技能"),flow});
    add({ItemKind::Assist,Tr(L"BOSS 援护"),flow});
    add({ItemKind::Look,Tr(L"变身"),flow});
    add({ItemKind::HeroLook,Tr(L"变回主角"),flow});
    add({ItemKind::WeaponMove,Tr(L"武器派生"),flow});
    add({ItemKind::WeaponActions,Tr(L"武器派生修改"),flow});
    add({ItemKind::AirSet,Tr(L"空中派生"),flow});
    add({ItemKind::Damage,Tr(L"伤害倍率"),flow});
    add({ItemKind::Trim,Tr(L"裁剪"),flow});
    add({ItemKind::Group,Tr(L"组合"),flow});
    add({ItemKind::Teleport,Tr(L"瞬移"),flow});
    add({ItemKind::Hook,Tr(L"钩子"),flow});
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
        if (NpcCombo::PicksBossMove(node)||node.type==NodeType::Look||node.type==NodeType::WeaponMove||(ExecutionTrigger(node)&&search_.pick_route==-2)||
            (WeaponActionsNode(node)&&search_.pick_route<0)) {
            const std::string& want=NpcCombo::PicksBossMove(node)?(search_.pick_route==-2?node.ability:node.boss):node.type==NodeType::Look?node.boss:
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
        if (node.type==NodeType::BossEffect) {
            const int route=search_.pick_route;
            const std::string want_name=route==-1?node.boss:route==-2?node.ability:std::string();
            const int want=route==-3?NpcCombo::ClampEffectPlace(node.fx_place):route==-4?NpcCombo::ClampEffectDamage(node.fx_damage):
                           route==-5?NpcCombo::ClampEffectRadius(node.fx_radius):NpcCombo::ClampEffectHits(node.fx_hits);
            for (size_t r=0;r<search_.rows.size();++r) {
                if (search_.rows[r].header) continue;
                const Item& it=search_.items[search_.rows[r].item];
                if (route>=-2?it.ability==want_name:it.trigger==want) { search_.selected=static_cast<int>(r); break; }
            }
            search_.scroll=0; return;
        }
        if (AirNode(node)||(LaunchShown(node)&&search_.pick_route==-2)) {
            const int want=AirNode(node)?AirValueOf(node,search_.pick_route):NpcCombo::ClampLaunch(node.launch);
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
    if(search_input_){const int pad=m.S(6);SetWindowPos(search_input_,HWND_TOP,search_.edit.left+pad,search_.edit.top+m.S(3),search_.edit.right-search_.edit.left-2*pad,search_.edit.bottom-search_.edit.top-m.S(6),SWP_NOACTIVATE);ShowWindow(search_input_,search_.open?SW_SHOWNA:SW_HIDE);SyncSearchInput();}
}
void SearchEnsureVisible();
void OpenSearch(POINT at,int from,int into,int from_out) {
    if (!HasSkin()) return;
    CommitEdit();
    search_.open=true; search_.query.clear(); search_.from=from; search_.into=into; search_.from_out=from_out; search_.hot=-1; search_.keyed=false;
    search_.pick_node=-1; search_.pick_route=-1; search_.pick_part=0; search_.mod_from=-1; search_.mode=0;
    search_.x=ToCanvasX(at.x); search_.y=ToCanvasY(at.y);
    SearchItems(); SearchFilter(); SearchLayout();
    FocusSearch(); Invalidate();
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
// The 文件 button's list (mode 1), under the toolbar.
void OpenFileSearch(int mode) {
    const RECT canvas=canvas_;
    OpenSearch({canvas.right-M().S(330),canvas.top+M().S(8)},-1,-1,0);
    if (!search_.open) return;
    search_.mode=mode; files_=NpcCombo::GetFiles();
    SearchItems(); SearchFilter(); SearchLayout(); Invalidate();
}
void OpenFiles() { OpenFileSearch(1); }
// 派生页的技能伤害 (mode 4) / 投技伤害 (mode 5): the presets under the button, the value in use selected.
void OpenDamageSearch(int mode) {
    const RECT b=buttons_[static_cast<int>(mode==4?Button::SkillDamage:Button::GrabDamage)];
    OpenSearch({b.left,canvas_.top+M().S(8)},-1,-1,0);
    if (!search_.open) return;
    search_.mode=mode; SearchItems(); SearchFilter();
    const float skill=NpcCombo::ChainSkillDamage(); const NpcCombo::ChainGrab grab=NpcCombo::ChainGrabDamage();
    for (size_t r=0;r<search_.rows.size();++r) {
        if (search_.rows[r].header) continue;
        const Item& item=search_.items[search_.rows[r].item];
        const bool now=mode==4?std::fabs(item.value-skill)<1e-3f:item.trigger==grab.mode&&(grab.mode<=0||std::fabs(item.value-grab.value)<1e-3f);
        if (now) { search_.selected=static_cast<int>(r); break; }
    }
    SearchLayout(); SearchEnsureVisible(); Invalidate();
}
// The 招式来源 list (mode 6) beside the dropdown, on the source shown now.
void OpenSourceSearch() {
    OpenSearch({canvas_.right,library_boss_box_.top},-1,-1,0);
    if (!search_.open) return;
    search_.mode=6; SearchItems(); SearchFilter();
    const std::string now=LibraryBoss();
    for (size_t r=0;r<search_.rows.size();++r)
        if (!search_.rows[r].header&&search_.items[search_.rows[r].item].ability==now) { search_.selected=static_cast<int>(r); break; }
    SearchLayout(); SearchEnsureVisible(); Invalidate();
}
// The 派生表 list under the header, on the one edited now.
void OpenChainSearch() {
    CommitEdit();
    search_.open=true; search_.query.clear(); search_.from=-1; search_.into=-1; search_.from_out=0; search_.hot=-1; search_.keyed=false;
    search_.pick_node=-1; search_.pick_route=-1; search_.pick_part=0; search_.mod_from=-1; search_.mode=3;
    search_.x=ToCanvasX(header_.left); search_.y=ToCanvasY(canvas_.top+M().S(8));
    SearchItems(); SearchFilter();
    for (size_t r=0;r<search_.rows.size();++r)
        if (!search_.rows[r].header&&search_.items[search_.rows[r].item].trigger==pinned_) { search_.selected=static_cast<int>(r); break; }
    SearchLayout(); SearchEnsureVisible(); FocusSearch(); Invalidate();
}
void CloseSearch() { if (search_.open) { search_.open=false;if(search_input_)ShowWindow(search_input_,SW_HIDE);if(!UiCursor::HandsOff())SetFocus(window_);Invalidate(); } }
// Windows' own Save As window, opened in combos\ on the active file's name with the .combo type. The page's search box
// uses a dedicated native edit for search now; Save As remains a standard system dialog. Modal to the
// game window (no clicks reach the game or the hub while it is open), with its own message loop on this thread; it has
// a caption, so the activation guard lets it take focus, and the game is active again when it closes.
// Empty: cancelled, or no dialog.
std::wstring AskSavePath() {
    const HRESULT com=CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED|COINIT_DISABLE_OLE1DDE);
    std::wstring picked;
    IFileSaveDialog* dialog=nullptr;
    if ((SUCCEEDED(com)||com==RPC_E_CHANGED_MODE)&&SUCCEEDED(CoCreateInstance(CLSID_FileSaveDialog,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&dialog)))) {
        const std::wstring kind=Tr(L"派生文件")+L" (*.combo)";
        const COMDLG_FILTERSPEC types[]={{kind.c_str(),L"*.combo"}};
        dialog->SetFileTypes(1,types); dialog->SetFileTypeIndex(1); dialog->SetDefaultExtension(L"combo");
        FILEOPENDIALOGOPTIONS options=0; dialog->GetOptions(&options);
        dialog->SetOptions(options|FOS_OVERWRITEPROMPT|FOS_FORCEFILESYSTEM|FOS_PATHMUSTEXIST|FOS_NOCHANGEDIR|FOS_STRICTFILETYPES);
        dialog->SetTitle(Tr(L"另存为").c_str());
        const NpcCombo::Files files=NpcCombo::GetFiles();
        if (!files.active.empty()) dialog->SetFileName(files.active.c_str());
        IShellItem* folder=nullptr;
        if (SUCCEEDED(SHCreateItemFromParsingName(NpcCombo::Folder().c_str(),nullptr,IID_PPV_ARGS(&folder)))) { dialog->SetFolder(folder); folder->Release(); }
        IShellItem* result=nullptr;
        if (SUCCEEDED(dialog->Show(window_?GetAncestor(window_,GA_ROOT):nullptr))&&SUCCEEDED(dialog->GetResult(&result))) {
            wchar_t* path=nullptr;
            if (SUCCEEDED(result->GetDisplayName(SIGDN_FILESYSPATH,&path))&&path) { picked=path; CoTaskMemFree(path); }
            result->Release();
        }
        dialog->Release();
    }
    if (SUCCEEDED(com)) CoUninitialize();
    return picked;
}
void OpenSaveAs() {
    CloseSearch(); CommitEdit(); Save();
    const std::wstring path=AskSavePath();
    if (path.empty()) return;
    std::wstring name;
    switch (NpcCombo::SaveToPath(path,name)) {
    case NpcCombo::SavedTo::Folder: DebugHub::Notify((Tr(L"已另存为：")+name+L".combo").c_str()); break;
    case NpcCombo::SavedTo::Copy: DebugHub::Notify((Tr(L"已另存一份到：")+path+Tr(L"（不在 combos 文件夹，当前文件不变）")).c_str()); break;
    default: DebugHub::Notify(Tr(L"文件名无效或无法写入，未保存").c_str()); break;
    }
    seen_generation_=NpcCombo::Generation(); Refresh(true);
}
// The modifiers an arrow from node `from`'s round pin, dropped on empty canvas, may point at.
void OpenModSearch(POINT at,int from) {
    OpenSearch(at,-1,-1,0);
    if (!search_.open) return;
    search_.mod_from=from;
    SearchItems(); SearchFilter(); SearchLayout(); Invalidate();
}
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
    search_.keyed=true; SearchEnsureVisible(); Invalidate();
}
// Adds the picked row's node where the search opened (wired when it came from a wire), or sets the picked button.
// Recording is a transaction: draft changes never reach runtime or disk before Enter.
void StopRecording(const wchar_t* note,bool commit=false) {
    if (record_.node<0) return;
    bool changed=false;
    if(commit&&At(record_.node)&&record_.generation==NpcCombo::Generation()){
        if(record_.route==kRecordThen){
            Node& node=config_.chain[static_cast<size_t>(record_.node)];
            if(RawTrigger(node)&&!record_.draft.empty()){const int value=NpcCombo::kThenRaw+record_.draft.front();changed=node.then!=value;node.then=value;}
        }else{
            const KeySpec spec=PickedKeys(record_.node,record_.route>=0?record_.route:-2,record_.route>=0?1:0);
            if(spec.keys&&spec.raw&&spec.keys==record_.target){changed=*spec.keys!=record_.draft;*spec.keys=record_.draft;}
        }
    }else if(commit)note=L"配置已变化，录制已取消";
    record_={};
    if(changed)Save();
    if(note)DebugHub::Notify(Tr(note).c_str());
    Layout();Invalidate();
}
void StartRecording(int node,int route=-1) {
    if (route==kRecordThen) { if (!At(node)||!RawTrigger(config_.chain[static_cast<size_t>(node)])) return; }
    else { const KeySpec spec=PickedKeys(node,route>=0?route:-2,route>=0?1:0); if (!spec.keys||!spec.raw) return; }
    StopRecording(nullptr);CloseSearch();CommitEdit();
    record_={};record_.node=node;record_.route=route;record_.generation=NpcCombo::Generation();
    if(route!=kRecordThen){const KeySpec spec=PickedKeys(node,route>=0?route:-2,route>=0?1:0);record_.target=spec.keys;record_.draft=*spec.keys;}
    {const UiHotkeys::PadLevel pad=UiHotkeys::CurrentPad();record_.pad_was=NpcCombo::PadCodeBits(pad.buttons,pad.stick);}
    record_.pad_seeded=true;
    if(IsWindowVisible(window_)&&!UiCursor::HandsOff())SetFocus(window_);
    DebugHub::Notify(Tr(L"录制中：键盘、鼠标或手柄（回车确认，Esc 取消）").c_str());
    Layout();Invalidate();
}
void RecordKey(int code) {
    if(record_.node<0||!At(record_.node)||!NpcCombo::ValidRawKey(code)||code==VK_RETURN||code==VK_ESCAPE)return;
    if(record_.route==kRecordThen)record_.draft={code};
    else if(record_.draft.size()<NpcCombo::kComboKeysMax)record_.draft.push_back(code);
    else{DebugHub::Notify(Tr(L"已录满 6 个键，回车确认或 Esc 取消").c_str());return;}
    Layout();Invalidate();
}
void SearchPick(int row) {
    if (row<0||static_cast<size_t>(row)>=search_.rows.size()||search_.rows[static_cast<size_t>(row)].header) return;
    const Item item=search_.items[search_.rows[static_cast<size_t>(row)].item];
    const float x=search_.x,y=search_.y; const int from=search_.from,into=search_.into,from_out=search_.from_out,pick=search_.pick_node,route=search_.pick_route,mod_from=search_.mod_from;
    const int part=search_.pick_part;
    CloseSearch();
    if (item.kind==ItemKind::MoveSource) {
        library_boss_=item.ability; category_=-1; library_pages_[4]=0;
        DebugHub::Notify((library_boss_.empty()?Tr(L"BOSS 招式列表：本表外观的招式"):
                          Tr(L"BOSS 招式列表：")+Tr(BossLookName(library_boss_).c_str())+Tr(L" 的技能（点击或拖到画布，加为 BOSS 技能）")).c_str());
        Layout(); Invalidate(); return;
    }
    if (item.kind==ItemKind::ChainSkill) {
        NpcCombo::SetChainSkillDamage(item.value);
        DebugHub::Notify((item.value>0?Tr(L"派生链放的 BOSS 技能伤害：")+ChainSkillText(item.value)+Tr(L"（代替 NPC 技能页的设置）"):
                                       Tr(L"派生链放的 BOSS 技能伤害跟随 NPC 技能页")).c_str());
        Layout(); Invalidate(); return;
    }
    if (item.kind==ItemKind::ChainGrab) {
        NpcCombo::SetChainGrabDamage({item.trigger,item.value});
        const NpcCombo::ChainGrab now=NpcCombo::ChainGrabDamage();
        DebugHub::Notify((now.mode!=NpcCombo::kGrabFollow?Tr(L"派生链放的投技伤害：")+ChainGrabText(now)+Tr(L"（代替投技页的设置）"):
                                                         Tr(L"派生链放的投技伤害跟随投技页")).c_str());
        Layout(); Invalidate(); return;
    }
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
    if (item.kind==ItemKind::BossLook&&At(pick)&&config_.chain[static_cast<size_t>(pick)].type==NodeType::Look) {
        config_.chain[static_cast<size_t>(pick)].boss=item.ability; Select(pick); Save(); Layout(); Invalidate(); return;
    }
    if (item.kind==ItemKind::BossLook||item.kind==ItemKind::BossMove) {
        if (!At(pick)||!NpcCombo::PicksBossMove(config_.chain[static_cast<size_t>(pick)])) return;
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
    if (item.kind==ItemKind::HookMode) {
        if (!At(pick)||config_.chain[static_cast<size_t>(pick)].type!=NodeType::Hook) return;
        if (item.trigger==2) NpcCombo::SetHookAuto(config_.chain[static_cast<size_t>(pick)]);
        else NpcCombo::SetHookMode(config_.chain[static_cast<size_t>(pick)],item.trigger==1);
        Select(pick); Save(); Layout(); Invalidate(); return;
    }
    if (item.kind==ItemKind::FxFolder||item.kind==ItemKind::FxPick||item.kind==ItemKind::FxPlace||item.kind==ItemKind::FxDamage||item.kind==ItemKind::FxRadius||item.kind==ItemKind::FxHits) {
        if (!At(pick)||config_.chain[static_cast<size_t>(pick)].type!=NodeType::BossEffect) return;
        Node& target=config_.chain[static_cast<size_t>(pick)];
        bool next=false;  // a new folder: its effect next
        switch (item.kind) {
        case ItemKind::FxFolder: {
            const NpcBossEffect* now=NpcBossEffects::Find(target.ability);
            if (target.boss!=item.ability) { target.boss=item.ability; if (!now||item.ability!=now->folder) { target.ability.clear(); next=true; } }
            break;
        }
        case ItemKind::FxPick: { const NpcBossEffect* e=NpcBossEffects::Find(item.ability); if (!e) return; target.ability=e->name; target.boss=e->folder; break; }
        case ItemKind::FxPlace: target.fx_place=NpcCombo::ClampEffectPlace(item.trigger); break;
        case ItemKind::FxDamage: {
            const int v=NpcCombo::ClampEffectDamage(item.trigger); if (v!=item.trigger) DebugHub::Notify(Tr(L"每段伤害在 0% 到 100% 之间，已调整。").c_str()); target.fx_damage=v; break; }
        case ItemKind::FxRadius: {
            const int v=NpcCombo::ClampEffectRadius(item.trigger); if (v!=item.trigger) DebugHub::Notify(Tr(L"伤害范围在 1 到 20 米之间，已调整。").c_str()); target.fx_radius=v; break; }
        default: {
            const int v=NpcCombo::ClampEffectHits(item.trigger); if (v!=item.trigger) DebugHub::Notify(Tr(L"段数在 1 到 10 之间，已调整。").c_str()); target.fx_hits=v; break; }
        }
        Select(pick); Save(); Layout(); Invalidate();
        if (next) OpenPicker(pick,-2);
        return;
    }
    if (item.kind==ItemKind::AirLift||item.kind==ItemKind::AirHang||item.kind==ItemKind::Launch) {
        if (!At(pick)) return;
        Node& target=config_.chain[static_cast<size_t>(pick)];
        if (item.kind==ItemKind::Launch) {
            if (target.type!=NodeType::ForceHit) return;
            const int value=NpcCombo::ClampLaunch(item.trigger);
            if (value!=item.trigger) DebugHub::Notify(Tr(L"击飞高度在 0 到 15 米之间，已调整。").c_str());
            target.launch=value;
        } else {
            if (!AirNode(target)) return;
            const bool hang=item.kind==ItemKind::AirHang;
            const int value=hang?NpcCombo::ClampAirHang(item.trigger):NpcCombo::ClampAirLift(item.trigger);
            if (value!=item.trigger) DebugHub::Notify(Tr(hang?L"滞空强度在 0% 到 100% 之间，已调整。":L"升高在 0 到 10 米之间，已调整。").c_str());
            AirValue(target,search_.pick_route)=value;
        }
        Select(pick); Save(); Layout(); Invalidate(); return;
    }
    if (item.kind==ItemKind::HookDist||item.kind==ItemKind::HookSpeed) {
        if (!At(pick)||config_.chain[static_cast<size_t>(pick)].type!=NodeType::Hook) return;
        Node& target=config_.chain[static_cast<size_t>(pick)];
        const bool speed=item.kind==ItemKind::HookSpeed;
        const int value=speed?NpcCombo::ClampHookSpeed(item.trigger):NpcCombo::ClampHookDist(item.trigger);
        if (value!=item.trigger) DebugHub::Notify(Tr(speed?L"钩子速度在 ×0.50 到 ×3.00 之间，已调整。":L"钩子最远距离在 3 到 30 米之间，已调整。").c_str());
        (speed?target.hook_speed:target.hook_dist)=value;
        Select(pick); Save(); Layout(); Invalidate(); return;
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
    case ItemKind::BoneTransform: node=NpcCombo::ModifierNode(NodeType::BoneTransform,0,0);node.bones.on=true;break;
    case ItemKind::Speed: node=NpcCombo::ModifierNode(NodeType::Speed,0,0); break;
    case ItemKind::Weapon: node=NpcCombo::ModifierNode(NodeType::Weapon,0,0); break;
    case ItemKind::Armor: node=NpcCombo::ModifierNode(NodeType::Armor,0,0); break;
    case ItemKind::Invincible: node=NpcCombo::ModifierNode(NodeType::Invincible,0,0); break;
    case ItemKind::IgnoreGuard: node=NpcCombo::ModifierNode(NodeType::IgnoreGuard,0,0); break;
    case ItemKind::KeepChain: node=NpcCombo::ModifierNode(NodeType::KeepChain,0,0); break;
    case ItemKind::KeepLook: node=NpcCombo::ModifierNode(NodeType::KeepLook,0,0); break;
    case ItemKind::ForceHit: node=NpcCombo::ModifierNode(NodeType::ForceHit,0,0); break;
    case ItemKind::Air: node=NpcCombo::ModifierNode(NodeType::Air,0,0); break;
    case ItemKind::BossEffect: node=NpcCombo::ModifierNode(NodeType::BossEffect,0,0); break;
    case ItemKind::Interrupt: node=NpcCombo::ModifierNode(NodeType::Interrupt,0,0); break;
    case ItemKind::Grab: node.type=NodeType::Grab; break;
    case ItemKind::BossSkill: node.type=NodeType::BossSkill; break;
    case ItemKind::Assist: node.type=NodeType::Assist; break;
    case ItemKind::Look: node.type=NodeType::Look; break;
    case ItemKind::HeroLook: node.type=NodeType::HeroLook; break;
    case ItemKind::WeaponMove: node.type=NodeType::WeaponMove; break;
    case ItemKind::Damage: node=NpcCombo::ModifierNode(NodeType::Damage,0,0); break;
    case ItemKind::Trim: node=NpcCombo::ModifierNode(NodeType::Trim,0,0); break;
    case ItemKind::Group: node.type=NodeType::Group; node.w=NpcCombo::kGroupNewW; node.h=NpcCombo::kGroupNewH; break;
    case ItemKind::Teleport: node.type=NodeType::Teleport; break;
    case ItemKind::Hook: node=NewHook(); break;
    case ItemKind::WeaponActions: node=NpcCombo::WeaponActionsNode(0,0); break;
    case ItemKind::AirSet: node=NpcCombo::AirSetNode(0,0); break;
    default: node.type=NodeType::Skill; node.ability=item.ability; break;
    }
    const bool exec=!NpcCombo::StartsChain(node)&&!NpcCombo::IsModifier(node);
    const int index=Place(node,x,y,exec?from:-1,exec?into:-1,from_out);
    if (index>=0&&mod_from>=0&&NpcCombo::LinkModifier(config_,mod_from,index)) { Save(); Invalidate(); }
    if (index>=0&&(node.type==NodeType::WeaponMove||node.type==NodeType::Grab||node.type==NodeType::Look||node.type==NodeType::Assist||node.type==NodeType::BossEffect||
                   (WeaponActionsNode(node)&&!node.air_set))) OpenPicker(index,-1);  // its weapon / grab / look at once
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
    case Button::Copy: CopySelection();return;
    case Button::Paste: PasteSelection();return;
    case Button::Toggle: config_.enabled=!config_.enabled; Save(); break;
    case Button::Toast: {
        const bool on=!NpcCombo::ShowToast(); NpcCombo::SetShowToast(on);
        DebugHub::Notify(Tr(on?L"派生提示已打开：派生链放招时右下角显示招式名":L"派生提示已关闭：派生链放招时右下角不再显示招式名").c_str());
        Layout(); Invalidate(); return;
    }
    case Button::Hud: {  // 派生显示: 关 -> 详细 -> 简略 -> 关
        const int mode=(NpcCombo::ChainHud()+1)%3; NpcCombo::SetChainHud(mode);
        DebugHub::Notify(Tr(mode==NpcCombo::kHudDetailed?L"派生显示：详细（右上角显示整条派生链的缩略图，当前位置实时高亮）":
                            mode==NpcCombo::kHudSimple?L"派生显示：简略（右上角显示接下来可以按的键和对应招式）":L"派生显示已关闭").c_str());
        Layout(); Invalidate(); return;
    }
    case Button::ViewTarget: {  // 镜头跟随: 骨盆 -> 脖子 -> 头部 -> 骨盆
        const int target=(NpcCombo::ViewTarget()+1)%3; NpcCombo::SetViewTarget(target);
        DebugHub::Notify(Tr(target==NpcCombo::kViewNeck?L"镜头跟随：脖子（招式中镜头盯着模型的脖子）":
                            target==NpcCombo::kViewHead?L"镜头跟随：头部（招式中镜头盯着模型的头部）":L"镜头跟随：骨盆（招式中镜头盯着模型的骨盆）").c_str());
        Layout(); Invalidate(); return;
    }
    case Button::SkillDamage: if (search_.open&&search_.mode==4) CloseSearch(); else OpenDamageSearch(4); return;
    case Button::GrabDamage: if (search_.open&&search_.mode==5) CloseSearch(); else OpenDamageSearch(5); return;
    case Button::Dummy: {  // 测试木桩: the game thread spawns / removes it; the label follows its snapshot (Tick)
        const bool present=TestDummy::GetSnapshot().present; TestDummy::Request(!present);
        if (!present) DebugHub::Notify(Tr(L"测试木桩：在主角面前召唤一个不移动、不攻击、锁血的小怪，用来试派生链").c_str());
        return;
    }
    case Button::Interval: { Node node; node.type=NodeType::Interval; node.ms=NpcCombo::kIntervalDefault; Append(node); return; }
    case Button::Branch: Append(NpcCombo::BranchNode(0,0)); return;
    case Button::BoneTransform: AddModifier(NodeType::BoneTransform);return;
    case Button::Speed: AddModifier(NodeType::Speed); return;
    case Button::Weapon: AddModifier(NodeType::Weapon); return;
    case Button::Armor: AddModifier(NodeType::Armor); return;
    case Button::Invincible: AddModifier(NodeType::Invincible); return;
    case Button::IgnoreGuard: AddModifier(NodeType::IgnoreGuard); return;
    case Button::KeepChain: AddModifier(NodeType::KeepChain); return;
    case Button::KeepLook: AddModifier(NodeType::KeepLook); return;
    case Button::ForceHit: AddModifier(NodeType::ForceHit); return;
    case Button::Air: AddModifier(NodeType::Air); return;
    case Button::BossEffect: AddModifier(NodeType::BossEffect); if (At(selected_)&&config_.chain[static_cast<size_t>(selected_)].type==NodeType::BossEffect) OpenPicker(selected_,-1); return;
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
    case Button::Assist: {
        // A BOSS 援护 after the selected move; its look is picked at once, then its move.
        const size_t before=config_.chain.size(); Node node; node.type=NodeType::Assist; Append(node);
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
    case Button::Trim: AddModifier(NodeType::Trim); return;  // linked to the selected BOSS 技能
    case Button::Group: { Node node; node.type=NodeType::Group; node.w=NpcCombo::kGroupNewW; node.h=NpcCombo::kGroupNewH; Append(node); return; }
    case Button::Teleport: { Node node; node.type=NodeType::Teleport; Append(node); return; }
    case Button::Hook: Append(NewHook()); return;  // 自动 first; its field switches it
    case Button::AutoDodge: {  // 按摇杆方向闪避: the look's dodge the stick points to when it plays (back when centred)
        if (moves_.dodges.empty()) { DebugHub::Notify(Tr(L"这个外观没有自己的闪避；按摇杆方向闪避只用于有闪避的 Boss 外观").c_str()); return; }
        Node node; node.type=NodeType::AutoDodge; Append(node); return;
    }
    case Button::File: OpenFiles(); return;
    case Button::SaveFile:  // the active file again; no active file, or it cannot be written: Windows' Save As window
        if (HasSkin()) NpcCombo::Save(moves_.profile,config_);
        files_=NpcCombo::GetFiles();
        if (files_.active.empty()||!NpcCombo::SaveFile()) { DebugHub::Notify(StatusText(NpcCombo::GetFiles().status).c_str()); OpenSaveAs(); return; }
        DebugHub::Notify((Tr(L"已保存：")+files_.active+L".combo").c_str());
        seen_generation_=NpcCombo::Generation(); return;
    case Button::SaveAs: OpenSaveAs(); return;
    case Button::AddTrigger: AddTrigger(); return;
    case Button::AirSet: AddStart(NpcCombo::AirSetNode(24,24)); return;  // a 空中派生 below everything
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
        keep.next=-1; keep.mods.fill(-1); config_.chain.assign(1,keep); CloseCurve("chain cleared"); Select(-1); Save(); break;
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
    case Part::Pill: if (hit.index>=0&&static_cast<size_t>(hit.index)<pills_.size()) Append(LibraryNode(pills_[static_cast<size_t>(hit.index)].ability)); return;
    case Part::LibraryBoss: if (search_.open&&search_.mode==6) CloseSearch(); else OpenSourceSearch(); return;
    case Part::LibraryTab: if (hit.index>=0&&hit.index<2) { library_tab_=hit.index; hot_={}; Layout(); } return;
    case Part::NodeCategory: if (hit.index>=0&&hit.index<5) { node_category_=hit.index; hot_={}; Layout(); } return;
    case Part::Tab: if (hit.index>=0&&static_cast<size_t>(hit.index)<tabs_.size()) { category_=tabs_[static_cast<size_t>(hit.index)].kind; library_pages_[4]=0; Layout(); } return;
    case Part::StripLeft: ScrollStrip(-1); return;
    case Part::StripRight: ScrollStrip(1); return;
    case Part::NodeValue: Select(hit.index); if (At(hit.index)&&Typed(config_.chain[static_cast<size_t>(hit.index)])) BeginEdit(hit.index); break;
    case Part::Node: case Part::NodeIn: case Part::NodeOut: case Part::ModOut: case Part::ModIn: case Part::RouteOut: case Part::RouteValue: Select(hit.index); break;
    case Part::NodeMinus: Select(hit.index); Nudge(hit.index,-1); break;
    case Part::NodePlus: Select(hit.index); Nudge(hit.index,1); break;
    case Part::RouteMinus: Select(hit.index); Nudge(hit.index,-1,hit.sub); break;
    case Part::NodeHoldMinus: Select(hit.index); NudgeHold(hit.index,-1,-1); break;
    case Part::NodeHoldPlus: Select(hit.index); NudgeHold(hit.index,1,-1); break;
    case Part::NodeHoldValue: Select(hit.index); BeginHoldEdit(hit.index,-1); break;
    case Part::RouteHoldMinus: Select(hit.index); NudgeHold(hit.index,-1,hit.sub); break;
    case Part::RouteHoldPlus: Select(hit.index); NudgeHold(hit.index,1,hit.sub); break;
    case Part::RouteHoldValue: Select(hit.index); BeginHoldEdit(hit.index,hit.sub); break;
    case Part::RoutePlus: Select(hit.index); Nudge(hit.index,1,hit.sub); break;
    case Part::RouteRemove: Select(hit.index); RemoveRoute(hit.index,hit.sub); break;
    case Part::RouteAdd: Select(hit.index); AddRoute(hit.index); break;
    case Part::RouteField: Select(hit.index); OpenPicker(hit.index,hit.sub); return;
    case Part::RouteKeys: Select(hit.index); OpenPicker(hit.index,hit.sub,1); return;
    case Part::RouteMode: Select(hit.index); OpenPicker(hit.index,hit.sub,2); return;
    case Part::NodeField: Select(hit.index); if(At(hit.index)&&At(hit.index)->type==NodeType::BoneTransform){OpenBones(hit.index);return;} if (At(hit.index)&&config_.chain[static_cast<size_t>(hit.index)].type==NodeType::Speed) { OpenCurve(hit.index); return; }
        if (!IsKey(hit.index)) OpenPicker(hit.index,-1); return;
    case Part::NodeField2: {
        Select(hit.index);
        const Node* n=At(hit.index);
        if (n&&n->type==NodeType::Speed) { OpenCurve(hit.index); return; }
        if (n&&((NpcCombo::PicksBossMove(*n)&&!n->boss.empty())||(n->type==NodeType::WeaponMove&&!n->weapon.empty())||ExecutionTrigger(*n)||KeysTrigger(*n)||
                 n->type==NodeType::Teleport||n->type==NodeType::Hook||AirNode(*n)||LaunchShown(*n)||n->type==NodeType::BossEffect)) OpenPicker(hit.index,-2);
        return;
    }
    case Part::NodeField3: Select(hit.index);
        if (At(hit.index)&&(KeysTrigger(config_.chain[static_cast<size_t>(hit.index)])||config_.chain[static_cast<size_t>(hit.index)].type==NodeType::Hook||
                            AirNode(config_.chain[static_cast<size_t>(hit.index)])||config_.chain[static_cast<size_t>(hit.index)].type==NodeType::BossEffect)) OpenPicker(hit.index,-3);
        return;
    case Part::NodeField4: Select(hit.index);
        if (At(hit.index)&&(KeysTrigger(config_.chain[static_cast<size_t>(hit.index)])||AirNode(config_.chain[static_cast<size_t>(hit.index)])||
                            config_.chain[static_cast<size_t>(hit.index)].type==NodeType::BossEffect)) OpenPicker(hit.index,-4);
        return;
    case Part::WeaponRow: AddWeaponNode(hit.index); return;
    case Part::NodeToggle:
        if (At(hit.index)&&config_.chain[static_cast<size_t>(hit.index)].type==NodeType::BossEffect) { Select(hit.index); OpenPicker(hit.index,-5); return; }  // its radius
        if (At(hit.index)&&config_.chain[static_cast<size_t>(hit.index)].type==NodeType::AirDefault) {  // 空中释放 on / off
            Select(hit.index); Node& n=config_.chain[static_cast<size_t>(hit.index)]; n.on=!n.on;
            DebugHub::Notify(Tr(n.on?L"空中释放已打开：空中放出的招式按它升高、滞空":L"空中释放已关闭：空中放出的招式照常下落").c_str());
            Save(); Layout(); break;
        }
        if (At(hit.index)&&NpcCombo::TakesOwnLook(config_.chain[static_cast<size_t>(hit.index)])) {
            Select(hit.index); Node& n=config_.chain[static_cast<size_t>(hit.index)]; n.own_look=!n.own_look; Save(); Layout(); break;
        }
        if (At(hit.index)&&config_.chain[static_cast<size_t>(hit.index)].type==NodeType::Speed) {
            Select(hit.index); Node& n=config_.chain[static_cast<size_t>(hit.index)];
            n.curve=!n.curve; if (n.curve&&n.points.size()<2) n.points=NpcCombo::DefaultCurve(n.speed);
            Save(); Layout(); break;
        }
        if (At(hit.index)) { Select(hit.index); config_.chain[static_cast<size_t>(hit.index)].on=!config_.chain[static_cast<size_t>(hit.index)].on; Save(); } break;
    case Part::NodeToggle2:
        if (At(hit.index)&&config_.chain[static_cast<size_t>(hit.index)].type==NodeType::BossEffect) { Select(hit.index); OpenPicker(hit.index,-6); return; }  // its hits
        if (At(hit.index)&&RawTrigger(config_.chain[static_cast<size_t>(hit.index)])) {
            Select(hit.index); Node& n=config_.chain[static_cast<size_t>(hit.index)]; n.swallow=!n.swallow; Save();
        }
        break;
    case Part::NodeToggle3:  // a chain start's 整体不变身
        if (At(hit.index)&&NpcCombo::StartsChain(config_.chain[static_cast<size_t>(hit.index)])) {
            Select(hit.index); Node& n=config_.chain[static_cast<size_t>(hit.index)]; n.own_look=!n.own_look;
            DebugHub::Notify(Tr(n.own_look?L"整体不变身已打开：这条链的 BOSS 技能、投技都套到当前外观上":L"整体不变身已关闭：各 BOSS 技能、投技按自己的设置").c_str());
            Save(); Layout();
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
    case Button::SkillDamage: { const float v=NpcCombo::ChainSkillDamage(); return Tr(L"技能伤害：")+(v>0?ChainSkillText(v):Tr(L"跟随"))+L" ▾"; }
    case Button::GrabDamage: { const NpcCombo::ChainGrab g=NpcCombo::ChainGrabDamage(); return Tr(L"投技伤害：")+(g.mode!=NpcCombo::kGrabFollow?ChainGrabText(g):Tr(L"跟随"))+L" ▾"; }
    case Button::Hud: { const int mode=NpcCombo::ChainHud(); return Tr(mode==NpcCombo::kHudDetailed?L"派生显示：详细":mode==NpcCombo::kHudSimple?L"派生显示：简略":L"派生显示：关"); }
    case Button::ViewTarget: { const int t=NpcCombo::ViewTarget(); return Tr(t==NpcCombo::kViewNeck?L"镜头跟随：脖子":t==NpcCombo::kViewHead?L"镜头跟随：头部":L"镜头跟随：骨盆"); }
    case Button::Dummy: return Tr(dummy_present_?L"移除测试木桩":L"召唤测试木桩");
    case Button::Interval: return Tr(L"+ 间隔");
    case Button::Branch: return Tr(L"+ 按键分支");
    case Button::BoneTransform: return Tr(L"+ 骨骼变换");
    case Button::Speed: return Tr(L"+ 加速");
    case Button::Weapon: return Tr(L"+ 武器属性");
    case Button::Armor: return Tr(L"+ 霸体");
    case Button::Invincible: return Tr(L"+ 无敌");
    case Button::IgnoreGuard: return Tr(L"+ 无视防御");
    case Button::KeepChain: return Tr(L"+ 保持派生");
    case Button::KeepLook: return Tr(L"+ 保留变身");
    case Button::ForceHit: return Tr(L"+ 强制受击");
    case Button::Air: return Tr(L"+ 滞空");
    case Button::BossEffect: return Tr(L"+ BOSS 特效");
    case Button::Interrupt: return Tr(L"+ 允许中断");
    case Button::Grab: return Tr(L"+ 投技");
    case Button::BossSkill: return Tr(L"+ BOSS 技能");
    case Button::Assist: return Tr(L"+ BOSS 援护");
    case Button::Look: return Tr(L"+ 变身");
    case Button::HeroLook: return Tr(L"+ 变回主角");
    case Button::WeaponMove: return Tr(L"+ 武器派生");
    case Button::WeaponActions: return Tr(L"+ 武器派生修改");
    case Button::AirSet: return Tr(L"+ 空中派生");
    case Button::Damage: return Tr(L"+ 伤害倍率");
    case Button::Trim: return Tr(L"+ 裁剪");
    case Button::Group: return Tr(L"+ 组合");
    case Button::Teleport: return Tr(L"+ 瞬移");
    case Button::Hook: return Tr(L"+ 钩子");
    case Button::AutoDodge: return Tr(L"+ 按摇杆方向闪避");
    case Button::File: return Tr(L"文件：")+(files_.active.empty()?std::wstring(L"-"):files_.active)+L" ▾";
    case Button::SaveFile: return Tr(L"保存");
    case Button::SaveAs: return Tr(L"另存为");
    case Button::AddTrigger: return Tr(L"+ 替换按键");
    case Button::Arrange: return Tr(L"整理");
    case Button::Frame: return Tr(L"显示全部");
    case Button::Copy: return Tr(L"复制");
    case Button::Paste: return Tr(L"粘贴");
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
    DropCanvasCache(); widths_.clear();
    if (!window_) return;
    RECT client{}; GetClientRect(window_,&client);
    const auto m=M(); const int pad=m.Margin(),gap=m.S(8),width=(std::max)(m.S(720),static_cast<int>(client.right));
    const int right=width-pad;
    const HDC dc=GetDC(window_);
    const HFONT body=Font(UiTheme::Font::Body),strong=Font(UiTheme::Font::Strong),small_font=Font(UiTheme::Font::Small);
    for (RECT& button:buttons_) button={};
    int y=m.S(12);
    const int row=m.S(32);
    const auto button_w=[&](Button b){ return static_cast<int>(Extent(dc,b==Button::Toggle?strong:small_font,ButtonText(b)).cx)+m.S(b==Button::Toggle?28:20); };
    const int toggle_w=button_w(Button::Toggle);
    buttons_[static_cast<int>(Button::Toggle)]={right-toggle_w,y,right,y+row};
    const int toast_w=button_w(Button::Toast);  // 派生提示: the notice in the game's bottom-right corner
    buttons_[static_cast<int>(Button::Toast)]={right-toggle_w-gap-toast_w,y,right-toggle_w-gap,y+row};
    const int hud_w=button_w(Button::Hud);  // 派生显示: the chain in the game's top-right corner
    buttons_[static_cast<int>(Button::Hud)]={right-toggle_w-toast_w-2*gap-hud_w,y,right-toggle_w-toast_w-2*gap,y+row};
    const int view_w=button_w(Button::ViewTarget);  // 镜头跟随: the bone the 视角跟随 camera looks at
    const int view_right=right-toggle_w-toast_w-hud_w-3*gap;
    buttons_[static_cast<int>(Button::ViewTarget)]={view_right-view_w,y,view_right,y+row};
    const int dummy_w=button_w(Button::Dummy);  // 测试木桩: a mob that stands still and does not die, to try chains on
    const int dummy_right=view_right-view_w-gap;
    buttons_[static_cast<int>(Button::Dummy)]={dummy_right-dummy_w,y,dummy_right,y+row};
    // 派生页的技能伤害 / 投技伤害: what the chain's BOSS 技能 and 投技 deal, over the other pages'.
    const int grab_w=button_w(Button::GrabDamage),skill_w=button_w(Button::SkillDamage),grab_right=dummy_right-dummy_w-gap;
    buttons_[static_cast<int>(Button::GrabDamage)]={grab_right-grab_w,y,grab_right,y+row};
    buttons_[static_cast<int>(Button::SkillDamage)]={grab_right-grab_w-gap-skill_w,y,grab_right-grab_w-gap,y+row};
    header_={pad,y,(std::max)(pad+m.S(80),grab_right-grab_w-skill_w-2*gap),y+row};
    header_.right=(std::min)(header_.right,pad+static_cast<LONG>(Extent(dc,HasSkin()?strong:body,HeaderText()).cx)+m.S(20));
    y+=row+m.S(10);
    const int footer_h=m.S(24),min_canvas=m.S(460),panel=m.S(264);
    canvas_={pad,y,right-panel-gap,(std::max)(y+min_canvas,static_cast<int>(client.bottom)-m.S(10)-footer_h-m.S(6))};
    library_={canvas_.right+gap,y,right,canvas_.bottom};
    footer_={pad,canvas_.bottom+m.S(6),right,canvas_.bottom+m.S(6)+footer_h};
    library_title_={library_.left+m.S(12),y+m.S(8),right-m.S(12),y+m.S(32)};
    const int left=library_.left+m.S(10),edge=right-m.S(10),half=(edge-left-gap)/2;
    int ly=library_title_.bottom+m.S(6);
    for (int i=0;i<2;++i) library_tabs_[i]={left+i*(half+gap),ly,left+i*(half+gap)+half,ly+m.S(32)};
    ly+=m.S(44);
    library_category_title_={};
    for (RECT& rect:node_categories_) rect={};
    if (library_tab_==0) {
        library_category_title_={left,ly,edge,ly+m.S(18)}; ly+=m.S(24);
        for (int i=0;i<5;++i) {
            const int x=left+(i%2)*(half+gap),top=ly+(i/2)*m.S(32);
            node_categories_[i]={x,top,i==4?edge:x+half,top+m.S(26)};
        }
        ly+=3*m.S(32)+m.S(4);
    }
    tabs_.clear(); pills_.clear(); weapon_rows_.clear(); weapons_={}; weapons_title_={}; tabs_title_={};
    const int pager_y=library_.bottom-m.S(38);
    strip_left_={left,pager_y,left+m.S(32),pager_y+m.S(28)};
    strip_right_={edge-m.S(32),pager_y,edge,pager_y+m.S(28)};
    library_page_label_={strip_left_.right+m.S(4),pager_y,strip_right_.left-m.S(4),pager_y+m.S(28)};
    std::vector<std::pair<Kind,std::string>> all;
    library_boss_box_={};
    if (HasSkin()&&LibrarySection()==4) {
        library_boss_box_={left,ly,edge,ly+m.S(28)}; ly+=m.S(34);
        NpcRetarget::SkinMoves other;const std::string boss=LibraryBoss();
        if (!boss.empty()&&NpcRetarget::GetSkinMoves(BossLookRow(boss),other))
            for (const std::string& ability:other.skills) all.push_back({NpcCombo::Classify(ability),ability});  // a BOSS 技能 takes no dodge
        else {
            for (const std::string& d:moves_.dodges) all.push_back({Kind::Dodge,d});
            for (const std::string& ability:moves_.skills) all.push_back({NpcCombo::Classify(ability),ability});
        }
        std::vector<Tab> wanted{{-1,{},all.size()}};
        for (int k=0;k<static_cast<int>(Kind::Count);++k) {
            const size_t count=static_cast<size_t>(std::count_if(all.begin(),all.end(),[k](const auto& a){return static_cast<int>(a.first)==k;}));
            if (count) wanted.push_back({k,{},count});
        }
        if (category_>=0&&std::none_of(wanted.begin(),wanted.end(),[](const Tab& t){return t.kind==category_;})) category_=-1;
        const int tab_w=(edge-left-m.S(6))/2;
        for (size_t i=0;i<wanted.size();++i) {
            const int x=left+static_cast<int>(i%2)*(tab_w+m.S(6)),top=ly+static_cast<int>(i/2)*m.S(28);
            wanted[i].rect={x,top,x+tab_w,top+m.S(24)};
        }
        ly+=static_cast<int>((wanted.size()+1)/2)*m.S(28)+m.S(6); tabs_=std::move(wanted);
        for (const auto& [kind,ability]:all) if (category_<0||static_cast<int>(kind)==category_) pills_.push_back({{},ability,kind});
    }
    library_section_={left,ly,edge,ly+m.S(24)}; ly+=m.S(30);
    library_list_={left,ly,edge,pager_y-m.S(8)};
    strip_={}; strip_area_={};
    const int row_h=m.S(34);
    library_capacity_=(std::max)(1,static_cast<int>((library_list_.bottom-library_list_.top)/row_h));
    std::vector<Button> creation;
    if (HasSkin()) for (auto b=std::rbegin(kTools);b!=std::rend(kTools);++b)
        if (NodeCategory(*b)==LibrarySection()) creation.push_back(*b);
    if (HasSkin()&&LibrarySection()==5) {
        for (const auto& [id,shaqi]:ListedWeapons()) {
            WeaponRow r; r.id=id; r.shaqi=shaqi; r.name=WeaponName(id);
            if (shaqi) r.name=Tr(L"影之武装")+L" · "+r.name;
            const std::wstring wide(id.begin(),id.end()); r.image=WeaponIcons::Find(wide.c_str(),shaqi).image;
            weapon_rows_.push_back(r);
        }
    }
    const size_t count=LibrarySection()==4?pills_.size():LibrarySection()==5?weapon_rows_.size():creation.size();
    library_items_=static_cast<int>(count);
    library_page_count_=(std::max)(1,(static_cast<int>(count)+library_capacity_-1)/library_capacity_);
    int& page=library_pages_[LibrarySection()]; page=std::clamp(page,0,library_page_count_-1);
    const size_t first=static_cast<size_t>(page*library_capacity_),last=(std::min)(count,first+library_capacity_);
    for (size_t i=first;i<last;++i) {
        const int top=ly+static_cast<int>(i-first)*row_h;
        const RECT rect{left,top,edge,top+row_h-m.S(4)};
        if (LibrarySection()==4) pills_[i].rect=rect;
        else if (LibrarySection()==5) weapon_rows_[i].rect=rect;
        else buttons_[static_cast<int>(creation[i])]=rect;
    }
    if (LibrarySection()==4) { strip_=library_list_; strip_area_=library_list_; }
    if (LibrarySection()==5) {
        weapons_=library_list_;
        weapons_title_={left,ly-m.S(4),edge,ly-m.S(4)};  // no repeated heading inside the list
    }
    // Compact management row only; node creation lives entirely in the library.
    const int tool_h=m.S(28),inset=m.S(10),first_limit=canvas_.right-m.S(100),rest=canvas_.right-inset;
    int x=canvas_.left+inset,top=canvas_.top+inset;
    for (Button b:kCanvasTools) {
        const int w=(std::min)(button_w(b),static_cast<int>(canvas_.right-canvas_.left-2*inset));
        const int limit=top==canvas_.top+inset?first_limit:rest;
        if (x+w>limit&&x>canvas_.left+inset) { x=canvas_.left+inset; top+=tool_h+m.S(6); }
        buttons_[static_cast<int>(b)]={x,top,x+w,top+tool_h}; x+=w+m.S(6);
    }
    const int minimum=y+min_canvas+m.S(6)+footer_h+m.S(10);
    const std::wstring key=UiHotkeys::Hint(UiHotkeys::Action::NpcSkill);
    wchar_t hint[1024]{};
    _snwprintf_s(hint,_countof(hint),_TRUNCATE,Tr(L"每按一次 %s（或被替换的主角按键）放下一招；招式中按下的键会记住，到它的派生点（未设则放完）时接上 · 按键分支：按不同的键、或在可接下一招后停顿再按，接不同的招 · 从起端节点或招式左侧的圆点拉箭头到加速 / 霸体 / 允许中断 / 武器属性：起端节点＝整条链，招式＝只这一招，武器属性只接起端节点 · 点加速的倍数或招式的派生点可直接输入 · Tab / 右键搜索添加 · 从引脚拉线连接，拉到空白处搜索并接上，Alt + 点引脚断开 · 空白处拖动平移，按住 Ctrl / Shift 拖动框选（选中的节点一起拖动、一起删除），Ctrl+C / Ctrl+V 复制粘贴，Ctrl + 点节点加选 / 取消 · 滚轮缩放 · Home 显示全部").c_str(),key.c_str());
    hint_text_=hint;
    ReleaseDC(window_,dc);
    if (search_.open) SearchLayout();
    if (minimum!=height_) { height_=minimum; DebugHub::SetMinimum(window_,m.S(720),height_); }
    Invalidate();
}
void ScrollStrip(int delta) { library_pages_[LibrarySection()]=std::clamp(library_pages_[LibrarySection()]+delta,0,library_page_count_-1); hot_={}; Layout(); }

// The two range handles use the same inset for paint, picking and dragging.
int TrimLength(int index) { return NpcCombo::ClampTrim(static_cast<int>(std::lround(TrimClip(static_cast<size_t>(index))*1000))); }
void TrimBounds(const Node& node,int clip,int& from,int& to) {
    from=NpcCombo::ClampTrim(node.trim_from);
    if (from>=clip) { from=0; to=clip; return; }
    to=node.trim_to>from?(std::min)(node.trim_to,clip):clip;
    if (to<=from) to=clip;
}
RECT TrimTrack(RECT bar) { InflateRect(&bar,-Px(6),0); return bar; }
bool UpdateTrimDrag(POINT p) {
    if (drag_.mode!=Drag::Trim||!At(drag_.index)||drag_.trim_clip<=0) return false;
    Node& node=config_.chain[static_cast<size_t>(drag_.index)];
    const RECT bar=drag_.trim_bar; const int clip=drag_.trim_clip,step=(std::min)(NpcCombo::kTrimStep,clip);
    int from=0,to=0; TrimBounds(node,clip,from,to);
    const double t=std::clamp(double(p.x-bar.left)/(std::max)(1L,bar.right-bar.left),0.0,1.0);
    const int ms=t>=1?clip:static_cast<int>(std::lround(t*clip/NpcCombo::kTrimStep))*NpcCombo::kTrimStep;
    if(drag_.sub>=2){const size_t at=static_cast<size_t>(drag_.sub-2);if(at>=node.trim_holds.size())return false;
        const int value=std::clamp(ms,from,(std::max)(from,to-1));const bool changed=node.trim_holds[at]!=value;node.trim_holds[at]=value;drag_.moved|=changed;drag_.last=p;Invalidate();return changed;}
    if (drag_.sub==0) from=std::clamp(ms,0,(std::max)(0,to-step));
    else to=std::clamp(ms,(std::min)(clip,from+step),clip);
    const int end=to>=clip?0:to;
    const bool changed=node.trim_from!=from||node.trim_to!=end;
    node.trim_from=from; node.trim_to=end; drag_.moved|=changed; drag_.last=p; Invalidate(); return changed;
}
bool PrepareTrimDrag(int index,POINT p) {
    if (!At(index)||At(index)->type!=NodeType::Trim) return false;
    const int clip=TrimLength(index); if (clip<=0) return false;
    const HDC dc=GetDC(window_); const RECT bar=TrimTrack(NodeShape(dc,static_cast<size_t>(index)).field2); ReleaseDC(window_,dc);
    if (bar.right<=bar.left) return false;
    int from=0,to=0; TrimBounds(*At(index),clip,from,to);
    const double start=bar.left+double(bar.right-bar.left)*from/clip,end=bar.left+double(bar.right-bar.left)*to/clip;
    drag_={}; drag_.mode=Drag::Trim; drag_.index=index; drag_.start=drag_.last=p; drag_.trim_bar=bar; drag_.trim_clip=clip;
    drag_.sub=std::fabs(p.x-start)<std::fabs(p.x-end)?0:1;
    for(size_t i=0;i<At(index)->trim_holds.size();++i){const double x=bar.left+double(bar.right-bar.left)*At(index)->trim_holds[i]/clip;if(std::fabs(p.x-x)<=Px(7)){drag_.sub=static_cast<int>(i)+2;break;}}
    hot_={Part::NodeTrimBar,index}; Select(index);
    if (drag_.sub<2&&(std::min)(std::fabs(p.x-start),std::fabs(p.x-end))>Px(8)) UpdateTrimDrag(p);
    return true;
}

// Right click empty timeline: add. Right click a marker: remove. Left drag moves it.
bool ToggleTrimHold(int index,POINT p){
    if(!At(index)||At(index)->type!=NodeType::Trim)return false;
    const int clip=TrimLength(index);if(clip<=0)return false;
    const HDC dc=GetDC(window_);const RECT bar=TrimTrack(NodeShape(dc,static_cast<size_t>(index)).field2);ReleaseDC(window_,dc);
    if(bar.right<=bar.left)return false;auto& node=config_.chain[static_cast<size_t>(index)];
    for(size_t i=0;i<node.trim_holds.size();++i){const double x=bar.left+double(bar.right-bar.left)*node.trim_holds[i]/clip;if(std::fabs(p.x-x)<=Px(7)){node.trim_holds.erase(node.trim_holds.begin()+static_cast<std::ptrdiff_t>(i));Save();Invalidate();return true;}}
    if(node.trim_holds.size()>=32){DebugHub::Notify(L"每个裁剪最多 32 个按住停留点。");return true;}
    int from=0,to=0;TrimBounds(node,clip,from,to);
    const double t=std::clamp(double(p.x-bar.left)/(bar.right-bar.left),0.0,1.0);
    const int ms=std::clamp(static_cast<int>(std::lround(t*clip/NpcCombo::kTrimStep))*NpcCombo::kTrimStep,from,(std::max)(from,to-1));
    node.trim_holds.push_back(ms);node.trim_holds=NpcCombo::TrimHolds(node.trim_holds);Save();Invalidate();return true;
}

const std::vector<Shape>& CanvasShapes(HDC dc){
    auto& cache=canvas_cache_;
    if(cache.shapes.size()!=config_.chain.size()||cache.pan_x!=pan_x_||cache.pan_y!=pan_y_||cache.scale!=Scale()||!EqualRect(&cache.canvas,&canvas_)){
        cache.shapes.clear();cache.shapes.reserve(config_.chain.size());
        for(size_t i=0;i<config_.chain.size();++i)cache.shapes.push_back(NodeShape(dc,i));
        cache.pan_x=pan_x_;cache.pan_y=pan_y_;cache.scale=Scale();cache.canvas=canvas_;++cache.shape_builds;
    }
    return cache.shapes;
}

// ---- hit testing ---------------------------------------------------------------------------------
bool Near(POINT a,POINT b,int r) { const int64_t x=int64_t(a.x)-b.x,y=int64_t(a.y)-b.y;return x*x+y*y<=int64_t(r)*r; }
Hit HitTest(POINT p) {
    const auto in=[&](const RECT& r){return PtInRect(&r,p)!=FALSE;};
    if (search_.open&&in(search_.rect)) return {Part::Search,SearchRowAt(p)};
    if (HasSkin()&&in(buttons_[static_cast<int>(Button::Toggle)])) return {Part::Button,static_cast<int>(Button::Toggle)};
    if (in(buttons_[static_cast<int>(Button::Toast)])) return {Part::Button,static_cast<int>(Button::Toast)};
    if (in(buttons_[static_cast<int>(Button::Hud)])) return {Part::Button,static_cast<int>(Button::Hud)};
    if (in(buttons_[static_cast<int>(Button::ViewTarget)])) return {Part::Button,static_cast<int>(Button::ViewTarget)};
    if (in(buttons_[static_cast<int>(Button::SkillDamage)])) return {Part::Button,static_cast<int>(Button::SkillDamage)};
    if (in(buttons_[static_cast<int>(Button::GrabDamage)])) return {Part::Button,static_cast<int>(Button::GrabDamage)};
    if (in(buttons_[static_cast<int>(Button::Dummy)])) return {Part::Button,static_cast<int>(Button::Dummy)};
    if (in(header_)) return {Part::Header,-1};
    for (int i=0;i<2;++i) if (in(library_tabs_[i])) return {Part::LibraryTab,i};
    for (int i=0;i<5;++i) if (in(node_categories_[i])) return {Part::NodeCategory,i};
    if (HasSkin()) for (const Button b:kTools) if (in(buttons_[static_cast<int>(b)])) return {Part::Button,static_cast<int>(b)};
    if (in(library_boss_box_)) return {Part::LibraryBoss,-1};
    for (size_t i=0;i<tabs_.size();++i) if (in(tabs_[i].rect)) return {Part::Tab,static_cast<int>(i)};
    if (library_pages_[LibrarySection()]>0&&in(strip_left_)) return {Part::StripLeft,-1};
    if (library_pages_[LibrarySection()]+1<library_page_count_&&in(strip_right_)) return {Part::StripRight,-1};
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
    const auto& order=PaintOrder();
    const auto& shapes=CanvasShapes(dc);
    for (size_t k=order.size();k-->0&&hit.part==Part::Canvas;) {
        const size_t n=order[k];
        const Node& node=config_.chain[n]; const Shape& s=shapes[n]; RECT grown=s.rect; InflateRect(&grown,pin,0);
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
        if (Overview()) { if (in(s.rect)) hit={Part::Node,i}; continue; }  // far out: the whole card (move, select)
        int route_out=-1;
        for (size_t r=0;r<s.routes.size();++r) if (Near(p,s.routes[r].out,pin)) { route_out=static_cast<int>(r); break; }
        const bool stepper=node.type==NodeType::Interval||node.type==NodeType::Trigger||node.type==NodeType::Speed||node.type==NodeType::Damage||node.type==NodeType::KeepChain||node.type==NodeType::Skill||
                           node.type==NodeType::AutoDodge||node.type==NodeType::BossSkill||node.type==NodeType::WeaponMove||node.type==NodeType::Teleport||node.type==NodeType::PreInput||
                           node.type==NodeType::WeaponActions||node.type==NodeType::Hook||node.type==NodeType::Trim;
        if (route_out>=0) hit={Part::RouteOut,i,route_out};
        else if (s.has_out&&Near(p,s.out,pin)) hit={Part::NodeOut,i};
        else if (s.has_in&&Near(p,s.in,pin)) hit={Part::NodeIn,i};
        else if (s.has_mod_out&&Near(p,s.mod_out,pin)) hit={Part::ModOut,i};
        else if (s.has_mod_in&&Near(p,s.mod_in,pin)) hit={Part::ModIn,i};
        else if (!in(s.rect)) continue;
        else if ((IsPicked(i)||hot_.index==i)&&!IsKey(i)&&!IsSettingAt(i)&&in(s.remove)) hit={Part::NodeRemove,i};
        else if (stepper&&in(s.minus)) hit={Part::NodeMinus,i};
        else if (stepper&&in(s.plus)) hit={Part::NodePlus,i};
        else if (stepper&&in(s.value)) hit={Part::NodeValue,i};
        else if (s.has_hold&&in(s.hminus)) hit={Part::NodeHoldMinus,i};
        else if (s.has_hold&&in(s.hplus)) hit={Part::NodeHoldPlus,i};
        else if (s.has_hold&&in(s.hvalue)) hit={Part::NodeHoldValue,i};
        else if (node.type==NodeType::Trim&&in(s.field2)) hit={Part::NodeTrimBar,i};
        else if (node.type==NodeType::Trigger&&!IsKey(i)&&in(s.field)) hit={Part::NodeField,i};
        else if (node.type==NodeType::Hook&&in(s.field)) hit={Part::NodeField,i};
        else if (node.type==NodeType::Hook&&in(s.field2)) hit={Part::NodeField2,i};
        else if (node.type==NodeType::Hook&&in(s.field3)) hit={Part::NodeField3,i};
        else if (node.type==NodeType::Speed&&in(s.toggle)) hit={Part::NodeToggle,i};
        else if (NpcCombo::TakesOwnLook(node)&&in(s.toggle)) hit={Part::NodeToggle,i};
        else if(node.type==NodeType::BoneTransform&&in(s.field))hit={Part::NodeField,i};
        else if (node.type==NodeType::Speed&&in(s.field)) hit={Part::NodeField,i};
        else if (node.type==NodeType::Speed&&node.curve&&in(s.field2)) hit={Part::NodeField2,i};
        else if (node.type==NodeType::Grab&&in(s.field)) hit={Part::NodeField,i};
        else if (node.type==NodeType::WeaponActions&&!node.air_set&&in(s.field)) hit={Part::NodeField,i};  // 空中派生: no weapon
        else if (node.type==NodeType::Look&&in(s.field)) hit={Part::NodeField,i};
        else if (node.type==NodeType::ForceHit&&in(s.field)) hit={Part::NodeField,i};
        else if (LaunchShown(node)&&in(s.field2)) hit={Part::NodeField2,i};
        else if (node.type==NodeType::BossEffect&&in(s.field)) hit={Part::NodeField,i};
        else if (node.type==NodeType::BossEffect&&in(s.field2)) hit={Part::NodeField2,i};
        else if (node.type==NodeType::BossEffect&&in(s.field3)) hit={Part::NodeField3,i};
        else if (node.type==NodeType::BossEffect&&in(s.field4)) hit={Part::NodeField4,i};
        else if (node.type==NodeType::BossEffect&&in(s.toggle)) hit={Part::NodeToggle,i};
        else if (node.type==NodeType::BossEffect&&in(s.toggle2)) hit={Part::NodeToggle2,i};
        else if (node.type==NodeType::AirDefault&&in(s.toggle)) hit={Part::NodeToggle,i};
        else if (AirNode(node)&&in(s.field)) hit={Part::NodeField,i};
        else if (AirNode(node)&&in(s.field2)) hit={Part::NodeField2,i};
        else if (AirNode(node)&&in(s.field3)) hit={Part::NodeField3,i};
        else if (AirNode(node)&&in(s.field4)) hit={Part::NodeField4,i};
        else if (NpcCombo::PicksBossMove(node)&&in(s.field)) hit={Part::NodeField,i};
        else if (NpcCombo::PicksBossMove(node)&&in(s.field2)) hit={Part::NodeField2,i};
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
        else if (NpcCombo::StartsChain(node)&&in(s.toggle3)) hit={Part::NodeToggle3,i};
        else if (NpcCombo::HasRows(node)) {
            hit={Part::Node,i};
            for (size_t r=0;r<s.routes.size();++r) {
                const RouteShape& route=s.routes[r]; const int row=static_cast<int>(r);
                if (in(route.field)) hit={Part::RouteField,i,row};
                else if (in(route.minus)) hit={Part::RouteMinus,i,row};
                else if (in(route.plus)) hit={Part::RoutePlus,i,row};
                else if (in(route.value)) hit={Part::RouteValue,i,row};
                else if (in(route.hminus)) hit={Part::RouteHoldMinus,i,row};
                else if (in(route.hplus)) hit={Part::RouteHoldPlus,i,row};
                else if (in(route.hvalue)) hit={Part::RouteHoldValue,i,row};
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
    {
        UiPaintBuffer::Frame buffer(window,target,width,height,printed?nullptr:&paint.rcPaint);
        const HDC surface=buffer.dc();
        if(surface){FillRect(surface,&client,UiTheme::Background());SetBkMode(surface,TRANSPARENT);
            Gdiplus::Graphics graphics(surface);graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);draw(surface,graphics,client);}
    }
    if (!printed) EndPaint(window,&paint);
}
void DrawButton(HDC dc,Gdiplus::Graphics& graphics,Button button,bool on=false,bool enabled=true) {
    const RECT& rect=buttons_[static_cast<int>(button)];
    if (rect.right<=rect.left||rect.bottom<=rect.top) return;
    const auto m=M(); const bool hot=enabled&&hot_==Hit{Part::Button,static_cast<int>(button)},down=hot&&pressed_==hot_;
    const COLORREF paper=Paper(),accent=UiTheme::Accent();
    COLORREF fill=on?UiTheme::Mix(paper,accent,40):UiTheme::Mix(paper,UiTheme::Base(),45),edge=on?accent:UiTheme::Mix(UiTheme::Border(),paper,35);
    if (NodeCategory(button)>=0) {
        const COLORREF row_fill=UiTheme::Mix(UiTheme::Base(),accent,hot?(down?20:12):4);
        Rounded(graphics,rect,static_cast<float>(m.S(5)),row_fill,255,hot?accent:UiTheme::Mix(UiTheme::Border(),UiTheme::Base(),55),255);
        RECT label=rect; label.left+=m.S(12); label.right-=m.S(10);
        Label(dc,ButtonText(button),label,Font(UiTheme::Font::Small),enabled?UiTheme::Text():UiTheme::MutedText(),DT_SINGLELINE|DT_VCENTER|DT_LEFT|DT_END_ELLIPSIS);
        return;
    }
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
    if (type==NodeType::IgnoreGuard) return UiTheme::Mix(RGB(120,150,240),UiTheme::Text(),15);
    if (type==NodeType::KeepChain) return UiTheme::Mix(RGB(64,176,214),UiTheme::Text(),15);
    if (type==NodeType::KeepLook) return UiTheme::Mix(RGB(168,120,232),UiTheme::Text(),15);
    if (type==NodeType::ForceHit) return UiTheme::Mix(RGB(236,128,48),UiTheme::Text(),15);
    if (type==NodeType::Interrupt) return UiTheme::Mix(RGB(70,190,150),UiTheme::Text(),15);
    if (type==NodeType::Damage) return UiTheme::Mix(RGB(226,72,112),UiTheme::Text(),15);
    if (type==NodeType::Trim) return UiTheme::Mix(RGB(172,196,64),UiTheme::Text(),15);
    if (type==NodeType::Air) return UiTheme::Mix(RGB(80,176,236),UiTheme::Text(),15);
    if (type==NodeType::BossEffect) return UiTheme::Mix(RGB(232,120,64),UiTheme::Text(),15);
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
// A checkbox row in a card: the box, then its words (muted while off).
void CheckRow(HDC dc,Gdiplus::Graphics& graphics,const RECT& row,bool on,bool hot,const std::wstring& words) {
    const COLORREF text=UiTheme::Text(),accent=UiTheme::Accent();
    const int box=row.bottom-row.top-Px(4);
    const RECT check{row.left,row.top+Px(2),row.left+box,row.top+Px(2)+box};
    Rounded(graphics,check,static_cast<float>(Px(4)),on?accent:UiTheme::Mix(UiTheme::Base(),text,hot?15:5),255,on?accent:UiTheme::Border(),255);
    if (on) {
        Gdiplus::Pen tick(Argb(UiTheme::Base()),(std::max)(1.5f,2.f*Zoom()));
        const float x=static_cast<float>(check.left),y=static_cast<float>(check.top),w=static_cast<float>(box);
        Gdiplus::PointF points[3]={{x+w*.22f,y+w*.52f},{x+w*.42f,y+w*.72f},{x+w*.78f,y+w*.3f}};
        graphics.DrawLines(&tick,points,3);
    }
    RECT label{check.right+Px(6),row.top,row.right,row.bottom};
    Label(dc,words,label,Scaled(UiTheme::Font::Small),on?text:UiTheme::MutedText(),DT_SINGLELINE|DT_VCENTER|DT_LEFT|DT_END_ELLIPSIS);
}
// A 加速's curve in its card (曲线调节): the curve over the row against ×1, its mean pace on the right.
void DrawCurveField(HDC dc,Gdiplus::Graphics& graphics,const RECT& rect,const Node& node,bool hot,bool open) {
    const COLORREF hue=ModColor(NodeType::Speed),text=UiTheme::Text();
    Rounded(graphics,rect,static_cast<float>(Px(5)),UiTheme::Mix(UiTheme::Base(),hue,hot?24:8),255,open?UiTheme::Accent():UiTheme::Mix(UiTheme::Border(),hue,30),255,open?1.6f:1.f);
    const std::wstring mean=SpeedText(static_cast<int>(std::lround(NpcCombo::CurveMeanSpeed(node.points))));
    const HDC measure=dc; const SIZE size=Extent(measure,Scaled(UiTheme::Font::Small),mean);
    RECT plot=rect; InflateRect(&plot,-Px(5),-Px(3)); plot.right-=size.cx+Px(8);
    RECT words=rect; words.left=plot.right+Px(4); words.right-=Px(6);
    Label(dc,mean,words,Scaled(UiTheme::Font::Small),text,DT_SINGLELINE|DT_VCENTER|DT_RIGHT);
    if (plot.right-plot.left<Px(20)) return;
    // log scale over the curve's own range (and ×1), so its shape shows in the small row
    double lo=100,hi=100;
    for (const NpcCombo::CurvePoint& p:node.points) { lo=(std::min)(lo,static_cast<double>(p.speed)); hi=(std::max)(hi,static_cast<double>(p.speed)); }
    const double log_lo=std::log(lo/1.15),log_hi=std::log(hi*1.15);
    const auto y_of=[&](double percent) {
        const double f=(std::log((std::max)(percent,1.0))-log_lo)/(std::max)(1e-6,log_hi-log_lo);
        return static_cast<float>(plot.bottom-std::clamp(f,0.0,1.0)*(plot.bottom-plot.top));
    };
    Gdiplus::Pen unit(Argb(UiTheme::Mix(UiTheme::Base(),text,30)),1.f);
    graphics.DrawLine(&unit,static_cast<float>(plot.left),y_of(100),static_cast<float>(plot.right),y_of(100));
    Gdiplus::PointF line[49];
    for (int k=0;k<49;++k) line[k]={static_cast<float>(plot.left+(plot.right-plot.left)*k/48.0),y_of(NpcCombo::CurveSpeed(node.points,k/48.0))};
    Gdiplus::Pen pen(Argb(hue),(std::max)(1.4f,1.8f*Zoom())); pen.SetLineJoin(Gdiplus::LineJoinRound);
    graphics.DrawLines(&pen,line,49);
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
    if(!search_input_)Label(dc,search_.query,input,Font(UiTheme::Font::Body),text,DT_SINGLELINE|DT_VCENTER|DT_LEFT|DT_END_ELLIPSIS);
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
// A card's colour, as its own drawing picks it (the overview's).
COLORREF NodeHue(const Node& node) {
    if (NpcCombo::IsModifier(node)) return ModColor(node.type);
    switch (node.type) {
    case NodeType::Trigger: return TriggerColor(node.trigger);
    case NodeType::Interval: return UiTheme::Mix(UiTheme::AccentBlue(),UiTheme::MutedText(),40);
    case NodeType::PreInput: case NodeType::AirDefault: return UiTheme::Mix(UiTheme::Accent(),UiTheme::MutedText(),30);
    case NodeType::Branch: return BranchColor();
    case NodeType::WeaponMove: case NodeType::WeaponActions: case NodeType::Hook: return WeaponColor();
    case NodeType::Teleport: case NodeType::HeroLook: return TeleportColor();
    case NodeType::Assist: return AssistColor();
    case NodeType::BossSkill: case NodeType::Look: return BossColor();
    case NodeType::Grab: return GrabColor();
    case NodeType::Group: return GroupColor();
    case NodeType::AutoDodge: return KindColor(Kind::Dodge);
    default: return KindColor(KindOf(node.ability));
    }
}
// Far out (under kOverviewZoom): a card is its tinted body and, while it is big enough to read, its title in the
// page's small font.
void OverviewCard(HDC dc,Gdiplus::Graphics& graphics,const Shape& s,COLORREF hue,const std::wstring& title,bool selected,bool playing,bool dim) {
    const auto m=M(); const COLORREF panel=UiTheme::Panel();
    const COLORREF edge=playing?UiTheme::AccentBlue():selected?UiTheme::Accent():UiTheme::Mix(UiTheme::Border(),hue,40);
    if (s.rect.bottom-s.rect.top<m.S(18)) {  // too small for a title: a flat box (GDI, quick for thousands)
        const HGDIOBJ old=SelectObject(dc,GetStockObject(DC_BRUSH));
        SetDCBrushColor(dc,UiTheme::Mix(UiTheme::Mix(panel,hue,dim?26:52),UiTheme::Base(),dim?30:0)); FillRect(dc,&s.rect,static_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
        if (selected||playing) { SetDCBrushColor(dc,edge); FrameRect(dc,&s.rect,static_cast<HBRUSH>(GetStockObject(DC_BRUSH))); }
        SelectObject(dc,old); return;
    }
    Rounded(graphics,s.rect,static_cast<float>(Px(8)),UiTheme::Mix(panel,hue,dim?26:52),dim?200:255,edge,255,selected||playing?2.f:1.f);
    if (s.rect.bottom-s.rect.top>=m.S(18)&&s.rect.right-s.rect.left>=m.S(36)) {
        RECT r=s.rect; InflateRect(&r,-m.S(4),0);
        Label(dc,title,r,Font(UiTheme::Font::Small),dim?UiTheme::MutedText():UiTheme::Text(),DT_SINGLELINE|DT_VCENTER|DT_CENTER|DT_END_ELLIPSIS);
    }
}
// A 裁剪's part on its move's length: the whole move a track, the part that plays filled, the times over it.
void TrimBar(HDC dc,Gdiplus::Graphics& graphics,const RECT& bar,size_t index,bool on) {
    const COLORREF hue=ModColor(NodeType::Trim);
    const float clip=TrimClip(index);
    Rounded(graphics,bar,static_cast<float>(Px(5)),UiTheme::Mix(UiTheme::Base(),hue,8),255,UiTheme::Mix(UiTheme::Border(),hue,30),255);
    if (clip>0) {
        NpcCombo::TrimRange r=NpcCombo::TrimOf(config_.chain[index]); if (r.from>=clip) r={};  // as the game plays it
        const double from=r.from,to=r.from+NpcCombo::TrimSpan(r,clip);
        const RECT track=TrimTrack(bar); const LONG w=track.right-track.left;
        RECT part{track.left+static_cast<LONG>(std::lround(w*from/clip)),bar.top+Px(3),track.left+static_cast<LONG>(std::lround(w*to/clip)),bar.bottom-Px(3)};
        if (part.right-part.left<Px(2)) part.right=part.left+Px(2);
        Rounded(graphics,part,static_cast<float>(Px(3)),hue,on?130:65,0,0);
        for (LONG x:{part.left,part.right}) {
            RECT handle{x-Px(4),bar.top+Px(2),x+Px(4),bar.bottom-Px(2)};
            Rounded(graphics,handle,static_cast<float>(Px(2)),UiTheme::Mix(hue,UiTheme::Text(),30),255,UiTheme::Base(),255);
        }
    }
    if(clip>0){const RECT track=TrimTrack(bar);int ordinal=0;
        for(int ms:NpcCombo::TrimHolds(config_.chain[index].trim_holds)){++ordinal;if(ms>clip*1000)continue;
            const LONG x=track.left+static_cast<LONG>(std::lround((track.right-track.left)*ms/(clip*1000.0)));
            const auto range=NpcCombo::TrimOf(config_.chain[index]);const bool enabled=ms>=range.from*1000&&ms<(range.from+NpcCombo::TrimSpan(range,clip))*1000;
            RECT mark{x-Px(7),bar.top+Px(1),x+Px(7),bar.bottom-Px(1)};
            Rounded(graphics,mark,static_cast<float>(Px(3)),enabled?UiTheme::Accent():UiTheme::MutedText(),255,UiTheme::Base(),255);
            Label(dc,std::to_wstring(ordinal),mark,Scaled(UiTheme::Font::Small),UiTheme::Base(),DT_CENTER|DT_SINGLELINE|DT_VCENTER);
        }
    }
    RECT inner=bar; InflateRect(&inner,-Px(7),0);
    if (!(clip>0)) Label(dc,TrimBarText(index),inner,Scaled(UiTheme::Font::Small),UiTheme::MutedText(),DT_SINGLELINE|DT_VCENTER|DT_CENTER|DT_END_ELLIPSIS);
}
void DrawCanvas(HDC dc,Gdiplus::Graphics& graphics) {
    RECT dirty{};GetClipBox(dc,&dirty);RECT canvas_dirty{};if(!IntersectRect(&canvas_dirty,&dirty,&canvas_))return;
    const auto m=M(); const COLORREF base=UiTheme::Base(),border=UiTheme::Border(),text=UiTheme::Text(),muted=UiTheme::MutedText(),accent=UiTheme::Accent();
    Rounded(graphics,canvas_,static_cast<float>(m.S(10)),base,255,0,0);
    const int saved=SaveDC(dc);
    IntersectClipRect(dc,canvas_.left+1,canvas_.top+1,canvas_.right-1,canvas_.bottom-1);
    Gdiplus::GraphicsPath clip; RoundedPath(clip,static_cast<float>(canvas_.left),static_cast<float>(canvas_.top),static_cast<float>(canvas_.right-canvas_.left),static_cast<float>(canvas_.bottom-canvas_.top),static_cast<float>(m.S(10)));
    graphics.SetClip(&clip);
    // Grid: minor lines every 16 units, major every 128, as in the Blueprint editor, in the theme's border colour.
    // Far out a major line every 8 (they stay 12 px or more apart); the lines counted, not summed, so they stay put.
    {
        const float step=kGrid*Scale();
        const long every=kMajor*Scale()<12.f?kMajor*8L:static_cast<long>(kMajor);
        Gdiplus::Pen minor(Argb(UiTheme::Mix(base,border,16)),1.f),major(Argb(UiTheme::Mix(base,border,45)),1.f);
        for (long k=static_cast<long>(std::floor(-pan_x_/step))-1;;++k) {
            const float x=canvas_.left+pan_x_+static_cast<float>(k)*step; if (x>=canvas_.right) break;
            const bool big=(k*kGrid)%every==0;
            if (step>=6||big) graphics.DrawLine(big?&major:&minor,x,static_cast<float>(canvas_.top),x,static_cast<float>(canvas_.bottom));
        }
        for (long k=static_cast<long>(std::floor(-pan_y_/step))-1;;++k) {
            const float y=canvas_.top+pan_y_+static_cast<float>(k)*step; if (y>=canvas_.bottom) break;
            const bool big=(k*kGrid)%every==0;
            if (step>=6||big) graphics.DrawLine(big?&major:&minor,static_cast<float>(canvas_.left),y,static_cast<float>(canvas_.right),y);
        }
    }
    // Which nodes play: reached from a trigger that is on; a modifier when a node pointing at it does.
    const size_t count=config_.chain.size();
    auto& live=canvas_cache_.live;auto& fed=canvas_cache_.fed;auto& modded=canvas_cache_.modded;auto& pointed=canvas_cache_.pointed;
    if(!canvas_cache_.graph_valid){
    live.assign(count,false);fed.assign(count,false);modded.assign(count,false);pointed.assign(count,false);
    for (size_t t=0;t<count;++t) {
        const Node& n=config_.chain[t];
        if (!NpcCombo::StartsChain(n)) continue;
        live[t]=WeaponActionsNode(n)?n.on&&(n.air_set||!n.weapon.empty()):n.on||IsKey(static_cast<int>(t));
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
        canvas_cache_.graph_valid=true;++canvas_cache_.graph_builds;
    }
    const auto& shapes=CanvasShapes(dc);
    const auto& order=PaintOrder();
    // Only what reaches the canvas is drawn (a chain of thousands of nodes stays quick).
    const auto visible=[&](const RECT& r){ RECT overlap{}; return IntersectRect(&overlap,&r,&canvas_dirty)!=FALSE; };
    const auto span=[](POINT a,POINT b,int pad){ return RECT{(std::min)(a.x,b.x)-pad,(std::min)(a.y,b.y)-pad,(std::max)(a.x,b.x)+pad,(std::max)(a.y,b.y)+pad}; };
    const bool overview=Overview();
    // 组合 frames under everything: a tinted body, the header with its pins, its 派生点, the size handle.
    for (const size_t i:order) {
        const Node& node=config_.chain[i]; if (node.type!=NodeType::Group) continue;
        const Shape& s=shapes[i]; const int index=static_cast<int>(i);
        if (!visible(s.rect)) continue;
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
            const POINT tip=ArrowTip(shapes[i].mod_out,target.rect,target.mod_in);
            if (!visible(span(shapes[i].mod_out,tip,Px(16)))) continue;
            Arrow(graphics,shapes[i].mod_out,tip,ModColor(config_.chain[static_cast<size_t>(mod)].type),live[i]?235:140,wire_w,part);
        }
    };
    const HGDIOBJ old_pen=SelectObject(dc,GetStockObject(DC_PEN));
    const auto flat_line=[&](POINT a,POINT b,COLORREF color){ SetDCPenColor(dc,color); MoveToEx(dc,a.x,a.y,nullptr); LineTo(dc,b.x,b.y); };
    if (overview) {
        for (size_t i=0;i<count;++i) for (int slot=0;slot<NpcCombo::kModSlots;++slot) {
            const int mod=NpcCombo::ModOf(config_,static_cast<int>(i),slot);
            if (mod<0||!shapes[i].has_mod_out) continue;
            const POINT from_mid{(shapes[i].rect.left+shapes[i].rect.right)/2,(shapes[i].rect.top+shapes[i].rect.bottom)/2};
            const RECT& to_rect=shapes[static_cast<size_t>(mod)].rect; const POINT to_mid{(to_rect.left+to_rect.right)/2,(to_rect.top+to_rect.bottom)/2};
            if (visible(span(from_mid,to_mid,2))) flat_line(from_mid,to_mid,UiTheme::Mix(base,ModColor(config_.chain[static_cast<size_t>(mod)].type),live[i]?70:35));
        }
    } else arrows(ArrowPart::Shaft);
    for (size_t i=0;i<count;++i) {
        const Node& n=config_.chain[i];
        for (size_t o=0;o<NpcCombo::OutCount(n);++o) {
            const int next=NpcCombo::OutOf(n,o); if (next<0||static_cast<size_t>(next)>=count) continue;
            const POINT from=NpcCombo::HasRows(n)&&o<shapes[i].routes.size()?shapes[i].routes[o].out:shapes[i].out;
            const POINT into=shapes[static_cast<size_t>(next)].in;
            if (overview) { if (visible(span(from,into,2))) flat_line(from,into,UiTheme::Mix(base,live[i]&&live[static_cast<size_t>(next)]?text:muted,live[i]?70:40)); continue; }
            // The curve stays inside its control points' box.
            const int reach=Px(kPin)+static_cast<int>((std::max)(static_cast<float>(Px(50)),std::fabs(static_cast<float>(into.x-from.x))*.5f))+Px(4);
            if (!visible(span(from,into,reach))) continue;
            const bool on=live[i]&&live[static_cast<size_t>(next)];
            Wire(graphics,from,into,on?text:muted,on?230:150,wire_w);
        }
    }
    for (const size_t i:order) {
        const Node& node=config_.chain[i]; const Shape& s=shapes[i]; const int index=static_cast<int>(i);
        if (node.type==NodeType::Group) continue;  // drawn above, under everything
        if (!visible(s.rect)) continue;
        const bool selected=IsPicked(index),playing=run_.running&&run_.profile==moves_.profile&&run_.node==index;
        const bool hot=hot_.index==index&&hot_.part>=Part::Node&&hot_.part<=Part::NodeRemove;
        const bool removable=!IsKey(index);
        if (overview) { OverviewCard(dc,graphics,s,NodeHue(node),CardTitle(i),selected,playing,NpcCombo::IsModifier(node)?!pointed[i]:!live[i]); continue; }
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
            if (s.has_hold) {  // 按住: how long the button is held before the chain starts
                RECT hold_label{s.rect.left+Px(10),s.hminus.top,s.hminus.left-Px(6),s.hminus.bottom};
                Label(dc,Tr(L"按住多久才触发"),hold_label,Scaled(UiTheme::Font::Small),muted,DT_SINGLELINE|DT_VCENTER|DT_LEFT|DT_END_ELLIPSIS);
                const bool editing=edit_.node==index&&edit_.hold==-1,caret=(GetTickCount64()/530)%2==0;
                StepperAt(dc,graphics,s.hminus,s.hvalue,s.hplus,editing?edit_.text+(caret?L"▏":L" ")+L" s":HoldText(node.hold_ms),
                          Hit{Part::NodeHoldMinus,index},Hit{Part::NodeHoldPlus,index},Hit{Part::NodeHoldValue,index});
                if (editing) Rounded(graphics,s.hvalue,static_cast<float>(Px(5)),0,0,accent,255,(std::max)(1.5f,2.f*Zoom()));
            }
            CheckRow(dc,graphics,s.toggle3,node.own_look,hot_==Hit{Part::NodeToggle3,index},ChainOwnLookText(node.own_look));
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
            Label(dc,ActionsWeaponText(node)+(node.air_set?L"":L"  ▾"),inner,Scaled(UiTheme::Font::Body),node.weapon.empty()&&!node.air_set?muted:text,
                  DT_SINGLELINE|DT_VCENTER|DT_LEFT|DT_END_ELLIPSIS);
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
                {const bool editing=edit_.node==index&&edit_.hold==row,caret=(GetTickCount64()/530)%2==0;
                    StepperAt(dc,graphics,rs.hminus,rs.hvalue,rs.hplus,editing?edit_.text+(caret?L"▏":L" ")+L" s":HoldText(route.hold_ms),Hit{Part::RouteHoldMinus,index,row},Hit{Part::RouteHoldPlus,index,row},Hit{Part::RouteHoldValue,index,row},UiTheme::Font::Small);
                    if(editing)Rounded(graphics,rs.hvalue,static_cast<float>(Px(5)),0,0,accent,255,(std::max)(1.5f,2.f*Zoom()));}
                const bool remove_hot=hot_==Hit{Part::RouteRemove,index,row};
                Rounded(graphics,rs.remove,static_cast<float>(Px(5)),remove_hot?UiTheme::Danger():UiTheme::Mix(UiTheme::Base(),UiTheme::Danger(),12),255,UiTheme::Mix(UiTheme::Border(),UiTheme::Danger(),30),255);
                Label(dc,L"×",rs.remove,Scaled(UiTheme::Font::Small),remove_hot?RGB(255,255,255):text,DT_SINGLELINE|DT_VCENTER|DT_CENTER);
                ExecPin(graphics,rs.out,route.next>=0,hot_==Hit{Part::RouteOut,index,row},live[i]?text:muted);
                if(rs.has_keys){const bool raw=RawRoute(route),recording=record_.node==index&&record_.route==row;
                    for(const auto& [rect,part,value]:{std::tuple<RECT,Part,std::wstring>{rs.keys,Part::RouteKeys,std::wstring()},std::tuple<RECT,Part,std::wstring>{rs.mode,Part::RouteMode,RouteModeText(route,true)}}){
                        Rounded(graphics,rect,static_cast<float>(Px(5)),UiTheme::Mix(UiTheme::Base(),hue,hot_==Hit{part,index,row}?28:10),255,UiTheme::Mix(UiTheme::Border(),hue,30),255);RECT inner2=rect;InflateRect(&inner2,-Px(6),0);
                        if(part==Part::RouteKeys)KeysField(dc,inner2,route.keys,route.chord,raw,recording,recording||NpcCombo::KeysRouteReady(route),UiTheme::Font::Small,text,muted);
                        else Label(dc,value+L" ▾",inner2,Scaled(UiTheme::Font::Small),text,DT_SINGLELINE|DT_VCENTER|DT_LEFT|DT_END_ELLIPSIS);}}
            }
            const bool add_hot=hot_==Hit{Part::RouteAdd,index};
            Rounded(graphics,s.add,static_cast<float>(Px(5)),UiTheme::Mix(UiTheme::Base(),accent,add_hot?24:6),255,UiTheme::Mix(UiTheme::Border(),accent,30),255);
            Label(dc,Tr(L"+ 添加动作"),s.add,Scaled(UiTheme::Font::Small),text,DT_SINGLELINE|DT_VCENTER|DT_CENTER);
            CheckRow(dc,graphics,s.toggle3,node.own_look,hot_==Hit{Part::NodeToggle3,index},ChainOwnLookText(node.own_look));
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
                {  // 按住: a tap, or how long the button is held for this row
                    const bool editing=edit_.node==index&&edit_.hold==row,caret=(GetTickCount64()/530)%2==0;
                    StepperAt(dc,graphics,rs.hminus,rs.hvalue,rs.hplus,editing?edit_.text+(caret?L"▏":L" ")+L" s":HoldText(route.hold_ms),Hit{Part::RouteHoldMinus,index,row},
                              Hit{Part::RouteHoldPlus,index,row},Hit{Part::RouteHoldValue,index,row},UiTheme::Font::Small);
                    if (editing) Rounded(graphics,rs.hvalue,static_cast<float>(Px(5)),0,0,accent,255,(std::max)(1.5f,2.f*Zoom()));
                }
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
        case NodeType::BoneTransform: {
            Card(dc,graphics,s,ModColor(node.type),CardTitle(i),selected,false,!pointed[i],selected||hot,hot_==Hit{Part::NodeRemove,index});
            std::wstring brief;for(size_t b=0;b<node.bones.rows.size()&&b<2;++b){if(b)brief+=L" · ";brief+=std::wstring(node.bones.rows[b].bone.begin(),node.bones.rows[b].bone.end());}
            if(node.bones.rows.size()>2)brief+=L" …";if(brief.empty())brief=L"尚未添加骨骼";
            Label(dc,brief,Row(s.rect,0),Scaled(UiTheme::Font::Small),text,DT_SINGLELINE|DT_VCENTER|DT_LEFT|DT_END_ELLIPSIS);
            Rounded(graphics,s.field,static_cast<float>(Px(4)),UiTheme::Mix(UiTheme::Base(),accent,15),255,UiTheme::Border(),255);
            Label(dc,L"详细编辑…",s.field,Scaled(UiTheme::Font::Small),text,DT_SINGLELINE|DT_VCENTER|DT_CENTER);Label(dc,NodeNote(i),Row(s.rect,2),Scaled(UiTheme::Font::Small),muted,DT_SINGLELINE|DT_VCENTER|DT_LEFT|DT_END_ELLIPSIS);break;
        }
        case NodeType::Speed: {
            Card(dc,graphics,s,ModColor(node.type),CardTitle(i),selected,false,!pointed[i],selected||hot,hot_==Hit{Part::NodeRemove,index});
            const COLORREF hue=ModColor(node.type);
            if (node.curve) DrawCurveField(dc,graphics,s.field2,node,hot_==Hit{Part::NodeField2,index},SpeedCurveWindow::EditedNode()==index);
            else {
                const bool editing=edit_.node==index;
                const bool caret=(GetTickCount64()/530)%2==0;
                Stepper(dc,graphics,s,index,editing?L"×"+edit_.text+(caret?L"▏":L" "):SpeedText(NpcCombo::ClampSpeed(node.speed)));
                if (editing) Rounded(graphics,s.value,static_cast<float>(Px(5)),0,0,accent,255,(std::max)(1.5f,2.f*Zoom()));
            }
            {  // 曲线调节 checkbox, 编辑曲线…
                const int box=s.toggle.bottom-s.toggle.top-Px(4);
                const RECT check{s.toggle.left,s.toggle.top+Px(2),s.toggle.left+box,s.toggle.top+Px(2)+box};
                const bool toggle_hot=hot_==Hit{Part::NodeToggle,index};
                Rounded(graphics,check,static_cast<float>(Px(4)),node.curve?accent:UiTheme::Mix(UiTheme::Base(),text,toggle_hot?15:5),255,node.curve?accent:UiTheme::Border(),255);
                if (node.curve) {
                    Gdiplus::Pen tick(Argb(UiTheme::Base()),(std::max)(1.5f,2.f*Zoom()));
                    const float x=static_cast<float>(check.left),y=static_cast<float>(check.top),w=static_cast<float>(box);
                    Gdiplus::PointF points[3]={{x+w*.22f,y+w*.52f},{x+w*.42f,y+w*.72f},{x+w*.78f,y+w*.3f}};
                    graphics.DrawLines(&tick,points,3);
                }
                RECT word{check.right+Px(6),s.toggle.top,s.toggle.right,s.toggle.bottom};
                Label(dc,Tr(L"曲线调节"),word,Scaled(UiTheme::Font::Small),node.curve?text:muted,DT_SINGLELINE|DT_VCENTER|DT_LEFT|DT_END_ELLIPSIS);
                const bool edit_hot=hot_==Hit{Part::NodeField,index};
                Rounded(graphics,s.field,static_cast<float>(Px(5)),UiTheme::Mix(UiTheme::Base(),hue,edit_hot?30:10),255,UiTheme::Mix(UiTheme::Border(),hue,30),255);
                Label(dc,Tr(L"编辑曲线…"),s.field,Scaled(UiTheme::Font::Small),text,DT_SINGLELINE|DT_VCENTER|DT_CENTER|DT_END_ELLIPSIS);
            }
            Label(dc,NodeNote(i),Row(s.rect,2),Scaled(UiTheme::Font::Small),muted,DT_SINGLELINE|DT_VCENTER|DT_LEFT|DT_END_ELLIPSIS);
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
        case NodeType::Trim: {  // the part of the BOSS 技能 pointing at it that plays
            Card(dc,graphics,s,ModColor(node.type),CardTitle(i),selected,false,!pointed[i],selected||hot,hot_==Hit{Part::NodeRemove,index});
            const bool caret=(GetTickCount64()/530)%2==0;
            const bool editing_from=edit_.node==index&&edit_.hold==-2,editing_to=edit_.node==index&&edit_.hold==-1;
            RECT from_label=Row(s.rect,0); from_label.right=s.minus.left-Px(6);
            Label(dc,Tr(L"开始于"),from_label,Scaled(UiTheme::Font::Small),muted,DT_SINGLELINE|DT_VCENTER|DT_LEFT|DT_END_ELLIPSIS);
            Stepper(dc,graphics,s,index,editing_from?edit_.text+(caret?L"▏":L" ")+L" s":Seconds(NpcCombo::ClampTrim(node.trim_from)/1000.0));
            if (editing_from) Rounded(graphics,s.value,static_cast<float>(Px(5)),0,0,accent,255,(std::max)(1.5f,2.f*Zoom()));
            RECT to_label{s.rect.left+Px(10),s.hminus.top,s.hminus.left-Px(6),s.hminus.bottom};
            Label(dc,Tr(L"结束于"),to_label,Scaled(UiTheme::Font::Small),muted,DT_SINGLELINE|DT_VCENTER|DT_LEFT|DT_END_ELLIPSIS);
            StepperAt(dc,graphics,s.hminus,s.hvalue,s.hplus,editing_to?edit_.text+(caret?L"▏":L" ")+L" s":TrimEndText(node),
                      Hit{Part::NodeHoldMinus,index},Hit{Part::NodeHoldPlus,index},Hit{Part::NodeHoldValue,index});
            if (editing_to) Rounded(graphics,s.hvalue,static_cast<float>(Px(5)),0,0,accent,255,(std::max)(1.5f,2.f*Zoom()));
            TrimBar(dc,graphics,s.field2,i,pointed[i]);
            Label(dc,TrimBarText(i),Row(s.rect,3),Scaled(UiTheme::Font::Small),text,DT_SINGLELINE|DT_VCENTER|DT_CENTER|DT_END_ELLIPSIS);
            Label(dc,TrimSourceText(i),Row(s.rect,4),Scaled(UiTheme::Font::Small),muted,DT_SINGLELINE|DT_VCENTER|DT_LEFT|DT_END_ELLIPSIS);
            Label(dc,NodeNote(i),Row(s.rect,5),Scaled(UiTheme::Font::Small),pointed[i]?text:muted,DT_SINGLELINE|DT_VCENTER|DT_LEFT|DT_END_ELLIPSIS);
            break;
        }
        case NodeType::KeepChain: {  // its own wait for the next press (in place of the trigger's reset time)
            Card(dc,graphics,s,ModColor(node.type),CardTitle(i),selected,false,!pointed[i],selected||hot,hot_==Hit{Part::NodeRemove,index});
            const bool editing=edit_.node==index;
            const bool caret=(GetTickCount64()/530)%2==0;
            Stepper(dc,graphics,s,index,editing?edit_.text+(caret?L"▏":L" ")+L" s":Seconds(NpcCombo::ClampKeepChain(node.ms)/1000.0));
            if (editing) Rounded(graphics,s.value,static_cast<float>(Px(5)),0,0,accent,255,(std::max)(1.5f,2.f*Zoom()));
            Label(dc,NodeNote(i),Row(s.rect,1),Scaled(UiTheme::Font::Small),pointed[i]?text:muted,DT_SINGLELINE|DT_VCENTER|DT_LEFT|DT_END_ELLIPSIS);
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
        case NodeType::Hook: {  // its mode, its reach, its speed, a note, its 派生点
            const COLORREF hue=WeaponColor();
            Card(dc,graphics,s,hue,CardTitle(i),selected,playing,!live[i],selected||hot,hot_==Hit{Part::NodeRemove,index});
            const auto field=[&](const RECT& r,Part part,const std::wstring& label){
                const bool field_hot=hot_==Hit{part,index};
                Rounded(graphics,r,static_cast<float>(Px(5)),UiTheme::Mix(UiTheme::Base(),hue,field_hot?28:10),255,UiTheme::Mix(UiTheme::Border(),hue,30),255);
                RECT inner=r; InflateRect(&inner,-Px(7),0);
                Label(dc,label+L"  ▾",inner,Scaled(UiTheme::Font::Body),text,DT_SINGLELINE|DT_VCENTER|DT_LEFT|DT_END_ELLIPSIS);
            };
            field(s.field,Part::NodeField,HookModeText(HookModeOf(node)));
            field(s.field2,Part::NodeField2,HookDistText(node.hook_dist));
            field(s.field3,Part::NodeField3,HookSpeedText(node.hook_speed));
            Label(dc,NodeNote(i),Row(s.rect,3,26),Scaled(UiTheme::Font::Small),muted,DT_SINGLELINE|DT_VCENTER|DT_LEFT|DT_END_ELLIPSIS);
            RECT label=Row(s.rect,4); label.right=s.minus.left-Px(6);
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
        case NodeType::Assist: {
            Card(dc,graphics,s,AssistColor(),CardTitle(i),selected,playing,!live[i],selected||hot,hot_==Hit{Part::NodeRemove,index});
            const auto field=[&](const RECT& r,Part part,const std::wstring& label,bool on){
                const bool field_hot=hot_==Hit{part,index};
                Rounded(graphics,r,static_cast<float>(Px(5)),UiTheme::Mix(UiTheme::Base(),AssistColor(),field_hot?28:10),255,UiTheme::Mix(UiTheme::Border(),AssistColor(),30),255);
                RECT inner=r; InflateRect(&inner,-Px(7),0);
                Label(dc,label+L"  ▾",inner,Scaled(UiTheme::Font::Body),on?text:muted,DT_SINGLELINE|DT_VCENTER|DT_LEFT|DT_END_ELLIPSIS);
            };
            field(s.field,Part::NodeField,node.boss.empty()?Tr(L"选择外观"):BossLookName(node.boss),!node.boss.empty());
            field(s.field2,Part::NodeField2,node.ability.empty()?Tr(L"选择招式"):MoveText(node.ability),!node.ability.empty());
            Label(dc,NodeNote(i),Row(s.rect,2),Scaled(UiTheme::Font::Small),muted,DT_SINGLELINE|DT_VCENTER|DT_LEFT|DT_END_ELLIPSIS);
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
            CheckRow(dc,graphics,s.toggle,node.own_look||ChainOwnLook(i),hot_==Hit{Part::NodeToggle,index},OwnLookText(i));
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
        case NodeType::BossEffect: {
            // BOSS 特效: its six fields (source, effect, place, damage, radius, hits), then a note.
            const COLORREF hue=ModColor(node.type);
            Card(dc,graphics,s,hue,CardTitle(i),selected,false,!pointed[i],selected||hot,hot_==Hit{Part::NodeRemove,index});
            const auto field=[&](const RECT& r,Part part,int route){
                const bool field_hot=hot_==Hit{part,index};
                const bool unset=(route==-1&&node.boss.empty())||(route==-2&&node.ability.empty());
                const bool off=route<=-5&&NpcCombo::ClampEffectDamage(node.fx_damage)==0;  // no damage: radius and hits unused
                Rounded(graphics,r,static_cast<float>(Px(5)),UiTheme::Mix(UiTheme::Base(),hue,field_hot?28:10),255,UiTheme::Mix(UiTheme::Border(),hue,30),255);
                RECT inner=r; InflateRect(&inner,-Px(7),0);
                Label(dc,FxFieldText(node,route)+L"  ▾",inner,Scaled(UiTheme::Font::Body),unset||off?muted:text,DT_SINGLELINE|DT_VCENTER|DT_LEFT|DT_END_ELLIPSIS);
            };
            field(s.field,Part::NodeField,-1); field(s.field2,Part::NodeField2,-2); field(s.field3,Part::NodeField3,-3);
            field(s.field4,Part::NodeField4,-4); field(s.toggle,Part::NodeToggle,-5); field(s.toggle2,Part::NodeToggle2,-6);
            Label(dc,NodeNote(i),Row(s.rect,6),Scaled(UiTheme::Font::Small),pointed[i]?text:muted,DT_SINGLELINE|DT_VCENTER|DT_LEFT|DT_END_ELLIPSIS);
            break;
        }
        case NodeType::ForceHit: case NodeType::Air: case NodeType::AirDefault: {
            // 强制受击: its kind (and 击飞's height); 滞空: its four values; 空中释放 (the chain's setting): its switch, then them.
            const bool setting=node.type==NodeType::AirDefault;
            const COLORREF hue=setting?UiTheme::Mix(UiTheme::Accent(),muted,30):ModColor(node.type);
            if (setting) Card(dc,graphics,s,hue,CardTitle(i),selected,false,!node.on,false,false);
            else Card(dc,graphics,s,hue,CardTitle(i),selected,false,!pointed[i],selected||hot,hot_==Hit{Part::NodeRemove,index});
            const auto field=[&](const RECT& r,Part part,const std::wstring& label){
                const bool field_hot=hot_==Hit{part,index};
                Rounded(graphics,r,static_cast<float>(Px(5)),UiTheme::Mix(UiTheme::Base(),hue,field_hot?28:10),255,UiTheme::Mix(UiTheme::Border(),hue,30),255);
                RECT inner=r; InflateRect(&inner,-Px(7),0);
                Label(dc,label+L"  ▾",inner,Scaled(UiTheme::Font::Body),setting&&!node.on?muted:text,DT_SINGLELINE|DT_VCENTER|DT_LEFT|DT_END_ELLIPSIS);
            };
            int note_row=1;
            if (node.type==NodeType::ForceHit) {
                field(s.field,Part::NodeField,Tr(NpcCombo::HitKindLabel(node.hit)));
                if (LaunchShown(node)) { field(s.field2,Part::NodeField2,LaunchText(node.launch)); note_row=2; }
            } else {
                if (setting) CheckRow(dc,graphics,s.toggle,node.on,hot_==Hit{Part::NodeToggle,index},Tr(L"启用"));
                field(s.field,Part::NodeField,AirText(-1,node.air_lift)); field(s.field2,Part::NodeField2,AirText(-2,node.air_hang));
                field(s.field3,Part::NodeField3,AirText(-3,node.air_foe_lift)); field(s.field4,Part::NodeField4,AirText(-4,node.air_foe_hang));
                note_row=setting?5:4;
            }
            Label(dc,NodeNote(i),Row(s.rect,note_row),Scaled(UiTheme::Font::Small),setting||pointed[i]?text:muted,DT_SINGLELINE|DT_VCENTER|DT_LEFT|DT_END_ELLIPSIS);
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
            CheckRow(dc,graphics,s.toggle,node.own_look||ChainOwnLook(i),hot_==Hit{Part::NodeToggle,index},OwnLookText(i));
            break;
        case NodeType::Armor: case NodeType::Interrupt: case NodeType::Invincible: case NodeType::KeepLook: case NodeType::IgnoreGuard:
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
        if(NpcCombo::HasAnimation(node)){
            RECT duration{s.rect.left+Px(10),s.rect.bottom-Px(kPad+kRow),s.rect.right-Px(10),s.rect.bottom-Px(kPad)};
            Label(dc,AnimationDuration(i),duration,Scaled(UiTheme::Font::Small),text,DT_SINGLELINE|DT_VCENTER|DT_LEFT|DT_END_ELLIPSIS);
        }
        if (s.has_in) ExecPin(graphics,s.in,fed[i],hot_==Hit{Part::NodeIn,index},live[i]?text:muted);
        if (s.has_mod_out) ModPin(graphics,s.mod_out,modded[i],hot_==Hit{Part::ModOut,index},UiTheme::AccentBlue());
        if (s.has_mod_in) ModPin(graphics,s.mod_in,pointed[i],hot_==Hit{Part::ModIn,index},ModColor(node.type));
        if (s.has_out) ExecPin(graphics,s.out,node.next>=0,hot_==Hit{Part::NodeOut,index},live[i]?text:muted);
    }
    SelectObject(dc,old_pen);
    // Arrow heads over every card: an arrow never ends under a node.
    if (!overview) arrows(ArrowPart::Head);
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
void DrawPreview(HDC dc,Gdiplus::Graphics& graphics,const RECT& client);
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
        DrawButton(dc,graphics,Button::Hud,NpcCombo::ChainHud()!=NpcCombo::kHudOff,true);
        DrawButton(dc,graphics,Button::ViewTarget,false,true);
        DrawButton(dc,graphics,Button::SkillDamage,NpcCombo::ChainSkillDamage()>0||(search_.open&&search_.mode==4),true);
        DrawButton(dc,graphics,Button::GrabDamage,NpcCombo::ChainGrabDamage().mode!=NpcCombo::kGrabFollow||(search_.open&&search_.mode==5),true);
        DrawButton(dc,graphics,Button::Dummy,dummy_present_,true);
        Rounded(graphics,library_,static_cast<float>(m.S(10)),UiTheme::Base(),255,UiTheme::Border(),255);
        Label(dc,Tr(L"添加到画布"),library_title_,strong,text,DT_SINGLELINE|DT_VCENTER|DT_LEFT);
        RECT tab_group=library_tabs_[0]; tab_group.right=library_tabs_[1].right; InflateRect(&tab_group,m.S(3),m.S(3));
        Rounded(graphics,tab_group,static_cast<float>(m.S(8)),UiTheme::Mix(paper,UiTheme::Base(),65),255,UiTheme::Border(),255);
        for (int i=0;i<2;++i) {
            const RECT& rect=library_tabs_[i]; const bool on=library_tab_==i,hot=hot_==Hit{Part::LibraryTab,i};
            Rounded(graphics,rect,static_cast<float>(m.S(6)),on?accent:UiTheme::Mix(paper,accent,hot?16:0),on||hot?255:0,0,0);
            Label(dc,Tr(i==0?L"节点列表":L"武器列表"),rect,strong,on?UiTheme::Base():hot?text:muted,DT_SINGLELINE|DT_VCENTER|DT_CENTER|DT_END_ELLIPSIS);
        }
        if (library_tab_==0) Label(dc,Tr(L"节点分类"),library_category_title_,small_font,muted,DT_SINGLELINE|DT_VCENTER|DT_LEFT);
        for (int i=0;i<5;++i) {
            const RECT& rect=node_categories_[i]; if (rect.right<=rect.left) continue;
            const bool on=node_category_==i,hot=hot_==Hit{Part::NodeCategory,i};
            Rounded(graphics,rect,static_cast<float>(m.S(4)),UiTheme::Mix(UiTheme::Base(),accent,on?20:hot?10:0),255,0,0);
            if (on) { RECT mark{rect.left,rect.top+m.S(5),rect.left+m.S(3),rect.bottom-m.S(5)}; Rounded(graphics,mark,1.f,accent,255,0,0); }
            RECT label=rect; label.left+=m.S(10); label.right-=m.S(4);
            Label(dc,Tr(kNodeCategories[i]),label,small_font,on?text:muted,DT_SINGLELINE|DT_VCENTER|DT_LEFT|DT_END_ELLIPSIS);
        }
        RECT section=library_section_,count=section; count.left=count.right-m.S(70); section.right=count.left-m.S(4);
        Label(dc,Tr(library_tab_==1?L"武器列表":kNodeCategories[node_category_]),section,small_font,text,DT_SINGLELINE|DT_VCENTER|DT_LEFT|DT_END_ELLIPSIS);
        wchar_t total[48]{}; _snwprintf_s(total,_countof(total),_TRUNCATE,Tr(L"共 %d 项").c_str(),library_items_);
        Label(dc,total,count,small_font,muted,DT_SINGLELINE|DT_VCENTER|DT_RIGHT|DT_END_ELLIPSIS);
        wchar_t page_text[64]{};
        _snwprintf_s(page_text,_countof(page_text),_TRUNCATE,Tr(L"第 %d / %d 页").c_str(),library_pages_[LibrarySection()]+1,library_page_count_);
        Label(dc,page_text,library_page_label_,small_font,text,DT_SINGLELINE|DT_VCENTER|DT_CENTER);
        if (library_boss_box_.right>library_boss_box_.left) {  // 招式来源
            const std::string boss=LibraryBoss();const bool hot=hot_.part==Part::LibraryBoss||(search_.open&&search_.mode==6);
            const COLORREF hue=boss.empty()?accent:BossColor();
            Rounded(graphics,library_boss_box_,static_cast<float>(m.S(6)),UiTheme::Mix(paper,hue,hot?24:boss.empty()?6:14),255,boss.empty()?UiTheme::Border():hue,255,boss.empty()?1.f:1.5f);
            RECT inner=library_boss_box_; InflateRect(&inner,-m.S(10),0);
            Label(dc,Tr(L"招式来源：")+(boss.empty()?Tr(L"本表外观"):Tr(BossLookName(boss).c_str()))+L"  ▾",inner,small_font,text,DT_SINGLELINE|DT_VCENTER|DT_LEFT|DT_END_ELLIPSIS);
        }
        for (size_t i=0;i<tabs_.size();++i) {
            const Tab& tab=tabs_[i]; const bool on=tab.kind==category_,hot=hot_==Hit{Part::Tab,static_cast<int>(i)};
            const COLORREF hue=tab.kind<0?accent:KindColor(static_cast<Kind>(tab.kind));
            Rounded(graphics,tab.rect,static_cast<float>(m.S(14)),UiTheme::Mix(paper,hue,on?38:hot?18:6),255,on?hue:UiTheme::Mix(UiTheme::Border(),paper,30),255,on?1.5f:1.f);
            Label(dc,TabText(tab),tab.rect,on?Font(UiTheme::Font::Strong):small_font,on?text:UiTheme::Mix(hue,text,35),DT_SINGLELINE|DT_VCENTER|DT_CENTER|DT_END_ELLIPSIS);
        }
        if (!HasSkin()) Label(dc,Tr(L"没有外观，没有可列出的招式。"),strip_,small_font,muted,DT_SINGLELINE|DT_VCENTER|DT_LEFT);
        {
            const int saved=SaveDC(dc);
            IntersectClipRect(dc,strip_area_.left,strip_area_.top,strip_area_.right,strip_area_.bottom);
            graphics.SetClip(Gdiplus::Rect(strip_area_.left,strip_area_.top,strip_area_.right-strip_area_.left,strip_area_.bottom-strip_area_.top));
            for (size_t i=0;i<pills_.size();++i) {
                const Pill& pill=pills_[i];
                if (pill.rect.right<=pill.rect.left||pill.rect.bottom<strip_area_.top||pill.rect.top>strip_area_.bottom) continue;
                const bool hot=hot_==Hit{Part::Pill,static_cast<int>(i)};
                const COLORREF hue=KindColor(pill.kind);
                Rounded(graphics,pill.rect,static_cast<float>(m.S(5)),UiTheme::Mix(paper,hue,hot?25:10),255,UiTheme::Mix(paper,hue,hot?90:40),255);
                RECT inner=pill.rect; InflateRect(&inner,-m.S(10),0);
                Label(dc,MoveText(pill.ability),inner,small_font,text,DT_SINGLELINE|DT_VCENTER|DT_LEFT|DT_END_ELLIPSIS);
            }
            graphics.ResetClip(); RestoreDC(dc,saved);
        }
        for (const auto& [rect,glyph,part]:{std::tuple{strip_left_,L"◀",Part::StripLeft},std::tuple{strip_right_,L"▶",Part::StripRight}}) {
            if (rect.right<=rect.left) continue;
            const bool enabled=part==Part::StripLeft?library_pages_[LibrarySection()]>0:library_pages_[LibrarySection()]+1<library_page_count_;
            Rounded(graphics,rect,static_cast<float>(m.S(6)),UiTheme::Mix(paper,accent,enabled&&hot_.part==part?18:0),enabled?255:100,enabled?UiTheme::Border():UiTheme::Base(),255);
            Label(dc,glyph,rect,small_font,enabled?text:UiTheme::Mix(muted,UiTheme::Base(),50),DT_SINGLELINE|DT_VCENTER|DT_CENTER);
        }
        if (HasSkin()) {
            LARGE_INTEGER hz{},t0{},t1{}; QueryPerformanceFrequency(&hz); QueryPerformanceCounter(&t0);
            DrawCanvas(dc,graphics);
            QueryPerformanceCounter(&t1);
            if (scratch_&&GetTickCount64()>=paint_logged_+1000) {  // a test chain shown: the canvas's paint time
                paint_logged_=GetTickCount64();
                Logf("COMBO_PAINT nodes=%zu zoom=%d%% overview=%d canvas=%.1f ms",config_.chain.size(),static_cast<int>(std::lround(Zoom()*100)),Overview()?1:0,
                     (t1.QuadPart-t0.QuadPart)*1000.0/hz.QuadPart);
            }
        }
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
                if (row.rect.right<=row.rect.left||row.rect.bottom<top||row.rect.top>weapons_.bottom) continue;
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
        const bool any=At(selected_)&&!IsKey(selected_)&&!IsSettingAt(selected_);
        if (HasSkin()) for (const Button b:kTools) DrawButton(dc,graphics,b,false,b==Button::AutoDodge?!moves_.dodges.empty():b==Button::Copy?!Selection().empty():b==Button::Paste?!node_clipboard_.empty():b!=Button::Remove||any);
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
        DrawPreview(dc,graphics,client);  // 招式预览, over everything
    });
}

// ---- input ---------------------------------------------------------------------------------------
// The applied skin, its moves and its saved chains; a new skin starts with nothing selected and the view framed.
void Refresh(bool force) {
    if(record_.node>=0){if(!force&&record_.generation==NpcCombo::Generation())return;StopRecording(nullptr);}
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
        skin_=want; scratch_=false; Select(-1); drag_={}; CloseSearch(); edit_={}; picked_.clear();
        if (!NpcRetarget::GetSkinMoves(skin_,moves_)) moves_={};
        config_=HasSkin()?NpcCombo::Load(moves_.profile):NpcCombo::Config{}; CloseCurve("another chain shown");
        seen_generation_=NpcCombo::Generation(); framed_=false; Layout();
    } else if (HasSkin()&&!scratch_&&NpcCombo::Generation()!=seen_generation_) {
        // The file changed (another file picked, or edited on disk): the chain as it is now.
        seen_generation_=NpcCombo::Generation(); config_=NpcCombo::Load(moves_.profile); CloseCurve("the chain file changed"); Select(-1); Layout();
    }
    if (skin_==-1&&GetTickCount64()>=hero_check_) {
        // The hero's moves are read in game every few seconds.
        hero_check_=GetTickCount64()+2000; NpcRetarget::SkinMoves fresh;
        if (NpcRetarget::GetSkinMoves(-1,fresh)&&fresh.skills!=moves_.skills) { moves_=fresh; Layout(); }
    }
    const NpcCombo::Run run=NpcCombo::GetRun();
    if (run.serial!=run_serial_) { run_serial_=run.serial; run_=run; Repaint(); }
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
    if (drag_.mode==Drag::Trim) { if (drag_.moved||Moved(p)) UpdateTrimDrag(p); return; }
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
    if(drag_.mode==Drag::Trim)Invalidate();else{canvas_cache_.Geometry();Repaint();}
}
void DragEnd(POINT p) {
    if (drag_.mode==Drag::Trim&&Moved(p)) UpdateTrimDrag(p);
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
    case Drag::Trim: if(At(done.index))config_.chain[static_cast<size_t>(done.index)].trim_holds=NpcCombo::TrimHolds(At(done.index)->trim_holds); Save(); Invalidate(); break;
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
            Place(LibraryNode(pills_[static_cast<size_t>(done.index)].ability),ToCanvasX(p.x)-30,ToCanvasY(p.y)-kHeader/2.f);
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
    const bool same_value=hit.index==edit_.node&&((hit.part==Part::NodeValue&&edit_.hold==-2)||(hit.part==Part::NodeHoldValue&&edit_.hold==-1)||
                                                  (hit.part==Part::RouteHoldValue&&edit_.hold>=0&&hit.sub==edit_.hold));
    if (edit_.node>=0&&!same_value) CommitEdit();  // a click elsewhere keeps the typed value
    pressed_=hit;
    switch (hit.part) {
    case Part::Search: if(hit.index<0)FocusSearch();else SearchPick(hit.index);return;
    case Part::NodeTrimBar: if (PrepareTrimDrag(hit.index,p)) SetCapture(window_); break;
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
    case VK_BACK: if (!search_.query.empty()) { search_.query.pop_back(); SearchFilter(); SyncSearchInput();Invalidate(); } return true;
    default: return true;  // every other key belongs to the search box while it is open
    }
}
bool WheelValue(const Hit& hit,int steps) {
    if (hit.part==Part::NodeValue||hit.part==Part::NodeMinus||hit.part==Part::NodePlus) Nudge(hit.index,steps);
    else if (hit.part==Part::NodeHoldValue||hit.part==Part::NodeHoldMinus||hit.part==Part::NodeHoldPlus) NudgeHold(hit.index,steps,-1);
    else if (hit.part==Part::RouteValue||hit.part==Part::RouteMinus||hit.part==Part::RoutePlus) Nudge(hit.index,steps,hit.sub);
    else if (hit.part==Part::RouteHoldValue||hit.part==Part::RouteHoldMinus||hit.part==Part::RouteHoldPlus) NudgeHold(hit.index,steps,hit.sub);
    else return false;
    return true;
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
        if (!(hit==hot_)) { hot_=hit; Repaint(); }
        if (search_.open) { const int row=SearchRowAt(p); if (row!=search_.hot) { search_.hot=row; if (row>=0) search_.keyed=false; Repaint(); } }
        TRACKMOUSEEVENT track{sizeof(track),TME_LEAVE,window,0}; TrackMouseEvent(&track);
        return 0;
    }
    case WM_MOUSELEAVE: if (drag_.mode==Drag::None) { hot_={}; search_.hot=-1; Repaint(); } return 0;
    case WM_SETCURSOR:
        if (LOWORD(lparam)==HTCLIENT) {
            const Part part=hot_.part;
            if (drag_.mode==Drag::Trim||part==Part::NodeTrimBar) { SetCursor(LoadCursorW(nullptr,IDC_SIZEWE)); return TRUE; }
            if (drag_.mode==Drag::Resize||part==Part::GroupResize) { SetCursor(LoadCursorW(nullptr,IDC_SIZENWSE)); return TRUE; }
            if (drag_.mode==Drag::Box) { SetCursor(LoadCursorW(nullptr,IDC_CROSS)); return TRUE; }
            if (drag_.mode==Drag::Pan||drag_.mode==Drag::Node) { SetCursor(LoadCursorW(nullptr,IDC_SIZEALL)); return TRUE; }
            if (part==Part::Node) { SetCursor(LoadCursorW(nullptr,IDC_SIZEALL)); return TRUE; }
            if (part!=Part::None&&part!=Part::Canvas) { SetCursor(LoadCursorW(nullptr,IDC_HAND)); return TRUE; }
        }
        break;
    case WM_LBUTTONDOWN: case WM_LBUTTONDBLCLK:
        if (record_.node>=0) { RecordKey(VK_LBUTTON); return 0; }
        PressAt({GET_X_LPARAM(lparam),GET_Y_LPARAM(lparam)},GetKeyState(VK_MENU)<0,wparam); return 0;
    case WM_XBUTTONDOWN: case WM_XBUTTONDBLCLK: case WM_XBUTTONUP:
        if (record_.node>=0) { if (message!=WM_XBUTTONUP) RecordKey(GET_XBUTTON_WPARAM(wparam)==XBUTTON1?VK_XBUTTON1:VK_XBUTTON2); return TRUE; }
        break;
    case WM_LBUTTONUP: pressed_={}; if (drag_.mode!=Drag::None&&!drag_.right) DragEnd({GET_X_LPARAM(lparam),GET_Y_LPARAM(lparam)}); return 0;
    case WM_RBUTTONDOWN: case WM_RBUTTONDBLCLK: case WM_MBUTTONDOWN: case WM_MBUTTONDBLCLK: {
        if (record_.node>=0) { RecordKey(message==WM_MBUTTONDOWN||message==WM_MBUTTONDBLCLK?VK_MBUTTON:VK_RBUTTON); return 0; }
        const POINT p{GET_X_LPARAM(lparam),GET_Y_LPARAM(lparam)};
        if(message==WM_RBUTTONDOWN){const Hit hit=HitTest(p);if(hit.part==Part::NodeTrimBar&&ToggleTrimHold(hit.index,p))return 0;}
        SetFocus(window);
        if (search_.open&&!PtInRect(&search_.rect,p)) CloseSearch();
        if (HasSkin()&&PtInRect(&canvas_,p)&&drag_.mode==Drag::None) BeginDrag(Drag::Pan,{},p,-1,message==WM_RBUTTONDOWN);
        return 0;
    }
    case WM_RBUTTONUP: case WM_MBUTTONUP: if (drag_.mode==Drag::Pan) DragEnd({GET_X_LPARAM(lparam),GET_Y_LPARAM(lparam)}); return 0;
    case WM_CAPTURECHANGED: if (drag_.mode!=Drag::None&&reinterpret_cast<HWND>(lparam)!=window) { const bool save=drag_.mode==Drag::Trim&&drag_.moved; drag_={}; if (save) Save(); Invalidate(); } return 0;
    case WM_MOUSEWHEEL: {
        if(record_.node>=0)return 0; // a wheel delta is not a held button
        POINT p{GET_X_LPARAM(lparam),GET_Y_LPARAM(lparam)}; ScreenToClient(window,&p);
        const int steps=GET_WHEEL_DELTA_WPARAM(wparam)>0?1:-1;
        if (search_.open&&PtInRect(&search_.rect,p)) {
            search_.scroll=std::clamp(search_.scroll-steps*3,0,(std::max)(0,static_cast<int>(search_.rows.size())-SearchVisible())); Invalidate(); return 0;
        }
        if (PtInRect(&library_,p)) { ScrollStrip(-steps); return 0; }
        if (!HasSkin()||!PtInRect(&canvas_,p)) break;  // the hub scrolls the page
        const Hit hit=HitTest(p);
        CommitEdit();
        if (!WheelValue(hit,steps)) ZoomAt(p,steps);
        return 0;
    }
    case WM_GETDLGCODE: return DLGC_WANTARROWS|DLGC_WANTTAB|DLGC_WANTCHARS|(search_.open||edit_.node>=0||record_.node>=0?DLGC_WANTALLKEYS:0);
    case WM_SYSKEYDOWN:
        if (record_.node>=0) { if(wparam==VK_ESCAPE)StopRecording(L"录制已取消");else if(wparam==VK_RETURN&&!(lparam&(1<<30)))StopRecording(L"录制已确认",true);else if (!(lparam&(1<<30))) RecordKey(static_cast<int>(wparam)); return 0; }
        break;
    case WM_CHAR:
        if (edit_.node>=0) { EditChar(static_cast<wchar_t>(wparam)); return 0; }
        if (search_.open&&wparam>=32&&wparam!=127) { search_.query.push_back(static_cast<wchar_t>(wparam)); SearchFilter(); SyncSearchInput();Invalidate(); }
        return 0;
    case WM_KEYDOWN:
        if (record_.node>=0) {
            if (wparam==VK_ESCAPE) StopRecording(L"录制已取消");
            else if(wparam==VK_RETURN&&!(lparam&(1<<30)))StopRecording(L"录制已确认",true);
            else if (!(lparam&(1<<30))) RecordKey(static_cast<int>(wparam));  // not a held key's repeat
            return 0;
        }
        if (EditKey(wparam)) return 0;
        if (SearchKey(wparam)) return 0;
        if(ClipboardKey(wparam,(GetKeyState(VK_CONTROL)&0x8000)!=0))return 0;
        if (wparam==VK_TAB) { POINT p=cursor_; if (!PtInRect(&canvas_,p)) p={(canvas_.left+canvas_.right)/2,(canvas_.top+canvas_.bottom)/3}; OpenSearch(p); return 0; }
        if (wparam==VK_DELETE||wparam==VK_BACK) { RemoveSelected(); return 0; }
        if (wparam==VK_HOME) { Frame(); return 0; }
        if ((wparam==VK_UP||wparam==VK_DOWN)&&At(selected_)) { Nudge(selected_,wparam==VK_UP?1:-1); return 0; }
        break;
    case WM_COMMAND: if(LOWORD(wparam)==kSearchInput&&HIWORD(wparam)==EN_CHANGE){if(!syncing_search_&&search_.open){search_.query=UiLayout::Caption(search_input_);SearchFilter();Invalidate();}return 0;} if (LOWORD(wparam)==IDCANCEL) { if (record_.node>=0) { StopRecording(L"录制已取消"); return 0; } if (edit_.node>=0) { CancelEdit(); return 0; } if (search_.open) { CloseSearch(); return 0; } DebugHub::Hide(); return 0; } break;
    case WM_CLOSE: StopRecording(nullptr);DebugHub::Hide(); return 0;
    case WM_DESTROY: record_={};window_=nullptr; return 0;
    default: break;
    }
    return DefWindowProcW(window,message,wparam,lparam);
}
}

bool HandleRecordingMessage(const MSG& message){
    if(record_.node<0||!window_)return false;
    if((message.message>=WM_KEYFIRST&&message.message<=WM_KEYLAST)||(message.message>=WM_MOUSEFIRST&&message.message<=WM_MOUSELAST)){
        SendMessageW(window_,message.message,message.wParam,message.lParam);return true;
    }
    return false;
}
bool OwnsTextMessage(const MSG& message){if(message.hwnd==window_&&message.message==WM_KEYDOWN&&(message.wParam=='C'||message.wParam=='V')&&(GetKeyState(VK_CONTROL)&0x8000)&&!search_.open&&edit_.node<0&&record_.node<0)return true;return search_.open&&search_input_&&message.hwnd==search_input_&&((message.message>=WM_KEYFIRST&&message.message<=WM_KEYLAST)||(message.message>=WM_IME_STARTCOMPOSITION&&message.message<=WM_IME_COMPOSITION));}
HWND CreatePage(HWND parent) {
    if (!parent) return nullptr;
    if (!window_) {
        WNDCLASSEXW cls{}; cls.cbSize=sizeof(cls); cls.hInstance=GetModuleHandleW(nullptr); cls.hCursor=LoadCursorW(nullptr,IDC_ARROW);
        cls.lpszClassName=kClass; cls.lpfnWndProc=&Proc;
        if (!RegisterClassExW(&cls)&&GetLastError()!=ERROR_CLASS_ALREADY_EXISTS) return nullptr;
        window_=CreateWindowExW(0,kClass,L"招式派生",WS_CHILD|WS_TABSTOP,0,0,800,650,parent,nullptr,GetModuleHandleW(nullptr),nullptr);
        if (!window_) return nullptr;
        search_input_=CreateWindowExW(0,L"EDIT",L"",WS_CHILD|WS_TABSTOP|ES_AUTOHSCROLL,0,0,10,24,window_,reinterpret_cast<HMENU>(static_cast<INT_PTR>(kSearchInput)),GetModuleHandleW(nullptr),nullptr);
        SendMessageW(search_input_,EM_SETLIMITTEXT,256,0);SendMessageW(search_input_,EM_SETCUEBANNER,TRUE,reinterpret_cast<LPARAM>(L"搜索招式、节点、按键…"));
        SendMessageW(search_input_,WM_SETFONT,reinterpret_cast<WPARAM>(UiTheme::GetFont(UiTheme::Font::Body,window_)),FALSE);SetWindowSubclass(search_input_,SearchInputProc,1,0);
        language_=UiI18n::GetLanguage();
        DebugHub::Attach(window_,parent,720,600);
    }
    Refresh(true); return window_;
}
namespace {
// A move row of the open search box: the one under the mouse, else the one picked with the keys. 招式 rows are this
// page's look's moves; a BOSS 技能 / 援护 node's 招式 rows are its look's.
// 投技 previews: a grab stands in the preview's look row as kGrabSkin with "grab:<variant id>" for its move.
constexpr int32_t kGrabSkin=-100;
constexpr int32_t kEffectSkin=-101;  // 特效预览: `ability` is a BOSS 特效's effect name
const char kGrabPrefix[]="grab:";
// The look a move of this page previews with: the page's own look, else (the hero's page, whose list holds the BOSS
// moves the hero was granted for 不变身 beside its own) the look whose moves hold that ability. -1: none (the hero's own
// moves have no preview). Found looks are kept; a miss is looked up again after 2 s (the looks' lists load late).
int32_t PreviewSkin(const std::string& ability) {
    if (skin_>=0||ability.empty()) return skin_;
    struct Found { int32_t row=-1; ULONGLONG at=0; };
    static std::unordered_map<std::string,Found> found;
    const ULONGLONG now=GetTickCount64();
    if (const auto it=found.find(ability);it!=found.end()&&(it->second.row>=0||now<it->second.at+2000)) return it->second.row;
    Found f; f.at=now;
    NpcRetarget::SkinMoves m;
    for (int32_t row=0;row<1024&&NpcRetarget::GetSkinMoves(row,m);++row)
        if (std::find(m.skills.begin(),m.skills.end(),ability)!=m.skills.end()) { f.row=row; break; }
    found[ability]=f; return f.row;
}
bool SearchMoveRow(int32_t& skin,std::string& ability,RECT& anchor) {
    const int row=search_.hot>=0?search_.hot:(search_.keyed?search_.selected:-1);
    if (row<0||static_cast<size_t>(row)>=search_.rows.size()||search_.rows[static_cast<size_t>(row)].header) return false;
    const size_t index=search_.rows[static_cast<size_t>(row)].item;
    if (index>=search_.items.size()||search_.items[index].ability.empty()) return false;
    const Item& item=search_.items[index];
    if (item.kind==ItemKind::Move) skin=PreviewSkin(item.ability);
    else if (item.kind==ItemKind::BossMove&&At(search_.pick_node)) skin=BossLookRow(config_.chain[static_cast<size_t>(search_.pick_node)].boss);
    else if (item.kind==ItemKind::GrabMove) skin=kGrabSkin;  // a 投技 node's list of grabs
    else if (item.kind==ItemKind::FxPick) skin=kEffectSkin;  // a BOSS 特效 node's list of effects
    else return false;
    const int row_h=M().S(26),top=search_.list.top+(row-search_.scroll)*row_h;
    if (row<search_.scroll||top+row_h>search_.list.bottom) return false;  // scrolled out of the list
    ability=skin==kGrabSkin?kGrabPrefix+item.ability:item.ability; anchor={search_.list.left,top,search_.list.right,top+row_h};
    return skin>=0||skin==kGrabSkin||skin==kEffectSkin;
}
// The move under the mouse: a pill of the strip (this page's look) or a move card (a BOSS 技能 / 援护 card: its own look),
// or a move row of the search box (beside).
// The move a 加速 node speeds up, for its preview: the first move pointing at it, else the first move of a trigger
// pointing at it (-1: none).
int MoveUnderSpeed(int speed_node) {
    const int slot=At(speed_node)&&At(speed_node)->type==NodeType::BoneTransform?NpcCombo::kModBone:NpcCombo::kModSpeed;
    const auto move=[&](int i){ const Node* n=At(i); return n&&((n->type==NodeType::Skill&&!n->ability.empty())||(NpcCombo::PicksBossMove(*n)&&!n->ability.empty()&&!n->boss.empty())); };
    for (size_t i=0;i<config_.chain.size();++i) if (config_.chain[i].mods[static_cast<size_t>(slot)]==speed_node&&move(static_cast<int>(i))) return static_cast<int>(i);
    for (size_t i=0;i<config_.chain.size();++i)
        if (config_.chain[i].mods[static_cast<size_t>(slot)]==speed_node&&NpcCombo::StartsChain(config_.chain[i])&&move(config_.chain[i].next)) return config_.chain[i].next;
    return -1;
}
bool HoverMove(int32_t& skin,std::string& ability,RECT& anchor,bool& beside,int& index) {
    beside=false; index=-1;
    if (test_hover_.on&&test_hover_.node>=0) {  // a BOSS 技能 card, or a 裁剪 card (its move)
        const Node* hovered=At(test_hover_.node);
        const int source=hovered&&hovered->type==NodeType::Trim?NpcCombo::TrimmedBy(config_,test_hover_.node):
            hovered&&(hovered->type==NodeType::BoneTransform||hovered->type==NodeType::Speed)?MoveUnderSpeed(test_hover_.node):test_hover_.node;
        const Node* n=At(source); if (!n||n->ability.empty()||(n->type!=NodeType::Skill&&!NpcCombo::PicksBossMove(*n))) return false;
        skin=n->type==NodeType::Skill?PreviewSkin(n->ability):BossLookRow(n->boss); ability=n->ability; index=source;
        const HDC dc=GetDC(window_); anchor=NodeShape(dc,static_cast<size_t>(test_hover_.node)).rect; ReleaseDC(window_,dc);
        return skin>=0;
    }
    if (test_hover_.on) {
        const LONG mid=(canvas_.left+canvas_.right)/2;
        skin=test_hover_.skin; ability=test_hover_.ability; anchor={mid-60,canvas_.top+M().S(8),mid+60,canvas_.top+M().S(40)};
        return true;
    }
    if ((drag_.mode!=Drag::None&&drag_.mode!=Drag::Trim)||edit_.node>=0||record_.node>=0) return false;
    if (search_.open) { beside=true; return SearchMoveRow(skin,ability,anchor); }
    if (hot_.part==Part::Pill&&hot_.index>=0&&static_cast<size_t>(hot_.index)<pills_.size()) {
        ability=pills_[static_cast<size_t>(hot_.index)].ability; const std::string boss=LibraryBoss();
        skin=boss.empty()?PreviewSkin(ability):BossLookRow(boss); anchor=pills_[static_cast<size_t>(hot_.index)].rect; return skin>=0&&!ability.empty(); }
    // A 加速 card (or its open curve window's node) shows the move it speeds up, at its speed.
    const int bone_editor=BoneMotionPage::EditedNode();
    const bool on_speed=hot_.index>=0&&At(hot_.index)&&(At(hot_.index)->type==NodeType::Speed||At(hot_.index)->type==NodeType::BoneTransform)&&
                        (hot_.part==Part::Node||hot_.part==Part::NodeField||hot_.part==Part::NodeField2||hot_.part==Part::NodeValue);
    const int editor=bone_editor>=0?bone_editor:SpeedCurveWindow::EditedNode();
    if(on_speed||(editor>=0&&hot_.part==Part::None&&At(editor))){
        const int from=on_speed?hot_.index:editor,move=MoveUnderSpeed(from);const Node* n=At(move);if(!n)return false;
        skin=n->type==NodeType::Skill?PreviewSkin(n->ability):BossLookRow(n->boss);ability=n->ability;index=move;
        const HDC dc=GetDC(window_);anchor=NodeShape(dc,static_cast<size_t>(from)).rect;ReleaseDC(window_,dc);return skin>=0;
    }
    // A 裁剪 card (its steppers too) shows the BOSS 技能 it cuts, only its part, as the chain plays it.
    if (hot_.index>=0&&At(hot_.index)&&config_.chain[static_cast<size_t>(hot_.index)].type==NodeType::Trim&&hot_.part>=Part::Node&&hot_.part<=Part::NodeTrimBar) {
        const int source=NpcCombo::TrimmedBy(config_,hot_.index);
        const Node* n=At(source); if (!n||n->ability.empty()) return false;
        skin=n->type==NodeType::Skill?PreviewSkin(n->ability):BossLookRow(n->boss); ability=n->ability; index=source;
        const HDC dc=GetDC(window_); anchor=NodeShape(dc,static_cast<size_t>(hot_.index)).rect; ReleaseDC(window_,dc);
        return skin>=0;
    }
    if (hot_.part!=Part::Node||!At(hot_.index)) return false;
    const Node& node=config_.chain[static_cast<size_t>(hot_.index)];
    index=hot_.index;
    if (node.type==NodeType::BossEffect&&!node.ability.empty()) {  // BOSS 特效: its effect on the stage
        skin=kEffectSkin; ability=node.ability; index=-1;
        const HDC dc=GetDC(window_); anchor=NodeShape(dc,static_cast<size_t>(hot_.index)).rect; ReleaseDC(window_,dc);
        return true;
    }
    if (node.type==NodeType::Grab&&!node.ability.empty()) {  // 投技: its grab
        skin=kGrabSkin; ability=kGrabPrefix+node.ability;
        const HDC dc=GetDC(window_); anchor=NodeShape(dc,static_cast<size_t>(hot_.index)).rect; ReleaseDC(window_,dc);
        return true;
    }
    if (node.type==NodeType::Skill&&!node.ability.empty()) { skin=PreviewSkin(node.ability); ability=node.ability; }
    else if (NpcCombo::PicksBossMove(node)&&!node.ability.empty()&&!node.boss.empty()) { skin=BossLookRow(node.boss); ability=node.ability; }
    else return false;
    const HDC dc=GetDC(window_); anchor=NodeShape(dc,static_cast<size_t>(hot_.index)).rect; ReleaseDC(window_,dc);
    return skin>=0;
}
void HidePreview() {
    if (preview_.asked) { MovePreview::Hide(); Invalidate(); }
    if (preview_hole_) { DebugHub::SetPreviewHole(nullptr); preview_hole_=false; preview_hole_rect_={}; }
    preview_={}; preview_frame_={}; preview_image_={};
}
// 实时预览: tells the game where the card's picture is (game client pixels) and, once the game draws the stage there
// itself (Frame::live), cuts the hub's hole over it. Without a hub that can have one: no place, read-back frames.
void SyncLiveView() {
    RECT screen{},client{};
    const bool want=preview_.asked&&!IsRectEmpty(&preview_image_)&&DebugHub::PreviewHoleSupported();
    if (want) {
        screen=preview_image_; MapWindowPoints(window_,nullptr,reinterpret_cast<POINT*>(&screen),2);
        const RECT game=UiHost::ClientScreen(); client=screen; OffsetRect(&client,-game.left,-game.top);
    }
    MovePreview::Place(client);
    const bool hole=want&&preview_frame_.live;
    if (hole!=preview_hole_||(hole&&!EqualRect(&screen,&preview_hole_rect_))) {
        DebugHub::SetPreviewHole(hole?&screen:nullptr); preview_hole_=hole; preview_hole_rect_=hole?screen:RECT{};
    }
}
void UpdatePreview() {
    int32_t skin=-1; std::string ability; RECT anchor{}; bool beside=false; int index=-1;
    if (!HoverMove(skin,ability,anchor,beside,index)) { if (preview_.asked||preview_.since) HidePreview(); return; }
    const ULONGLONG now=GetTickCount64();
    if (skin!=preview_.skin||ability!=preview_.ability) { HidePreview(); preview_.skin=skin; preview_.ability=ability; preview_.since=now; }
    // The move's 加速 (its curve too), as the chain plays it: a change goes to the playing preview at once.
    // Its 裁剪 too: only that part plays, the 加速 (and its curve) over it.
    int speed=100,trim_from=0,trim_to=0; std::vector<int> trim_holds;std::vector<NpcCombo::CurvePoint> curve;
    if (index>=0) {
        const int trigger=NpcCombo::TriggerOf(config_,static_cast<size_t>(index));
        const Node* mod=NpcCombo::SpeedNodeFor(config_,trigger,index);
        if (NpcCombo::CurveOn(mod)) curve=mod->points; else speed=NpcCombo::SpeedFor(config_,trigger,index);
        const NpcCombo::TrimRange trim=NpcCombo::TrimFor(config_,index);
        trim_holds=trim.holds;
        if (trim.on) { trim_from=static_cast<int>(std::lround(trim.from*1000)); trim_to=static_cast<int>(std::lround(trim.to*1000)); }
    }
    BoneMotion::Config bones;
    if(index>=0){const int mod=NpcCombo::ModFor(config_,NpcCombo::TriggerOf(config_,static_cast<size_t>(index)),index,NpcCombo::kModBone);if(mod>=0)bones=config_.chain[static_cast<size_t>(mod)].bones;}
    const bool changed=trim_holds!=preview_.trim_holds||BoneMotion::Encode(bones)!=BoneMotion::Encode(preview_.bones)||speed!=preview_.speed||trim_from!=preview_.trim_from||trim_to!=preview_.trim_to||curve.size()!=preview_.curve.size()||
                       !std::equal(curve.begin(),curve.end(),preview_.curve.begin(),[](const NpcCombo::CurvePoint& a,const NpcCombo::CurvePoint& b){ return a.t==b.t&&a.speed==b.speed; });
    preview_.trim_holds=trim_holds;preview_.bones=bones; preview_.node=index; preview_.speed=speed; preview_.curve=curve; preview_.trim_from=trim_from; preview_.trim_to=trim_to;
    if (!EqualRect(&anchor,&preview_.anchor)||beside!=preview_.beside) { preview_.anchor=anchor; preview_.beside=beside; if (preview_.asked) Invalidate(); }
    if (!preview_.asked&&!preview_.none&&now>=preview_.since+kPreviewDelayMs) {
        std::string monster,package;
        if (skin==kGrabSkin) {
            // 投技: the boss of the look that plays the grab, playing the grab's executor montage (no target to grab).
            const MotianGrab::Variant* v=NodeGrab([&]{ Node n; n.ability=ability.substr(std::strlen(kGrabPrefix)); return n; }());
            if (!v||!NpcRetarget::GrabPreviewMonster(v->boss_id,v->source,monster)||(package=MotianGrab::PreviewMontage(v->index)).empty()) { preview_.none=true; return; }
        } else if (skin==kEffectSkin) {
            const NpcBossEffect* e=NpcBossEffects::Find(ability); if (!e) { preview_.none=true; return; }
            monster=MovePreview::kEffectStage; package=e->path;
        } else if (!NpcRetarget::MovePreviewSubject(skin,ability,monster,package)) { preview_.none=true; return; }  // the hero's moves: none
        MovePreview::Show(monster,package,preview_.speed,preview_.curve,preview_.trim_from,preview_.trim_to,preview_.bones,preview_.trim_holds); preview_.asked=true; preview_.monster=monster; preview_.package=package; preview_frame_={}; Invalidate();
    } else if (preview_.asked&&changed&&!preview_.monster.empty()) {
        MovePreview::Show(preview_.monster,preview_.package,preview_.speed,preview_.curve,preview_.trim_from,preview_.trim_to,preview_.bones,preview_.trim_holds); Invalidate();
    }
    if (preview_.asked&&MovePreview::Latest(preview_frame_,preview_frame_.serial)) InvalidateRect(window_,&preview_rect_,FALSE);
    SyncLiveView();
}
// The card: the stage's picture (or what it is doing), the move's name under it; under its pill / card, or over it
// when there is no room below. For a search box row: by the box, on its side with more room (smaller, down to half
// size, to fit there), level with the row.
void DrawPreview(HDC dc,Gdiplus::Graphics& graphics,const RECT& client) {
    if (!preview_.asked) return;
    const auto m=M();
    const bool beside=preview_.beside&&search_.open;
    const int pad=m.S(6),caption=m.S(24),gap=m.S(6);
    const RECT& box=search_.rect;
    const int right_room=static_cast<int>(client.right)-gap-(static_cast<int>(box.right)+gap),left_room=static_cast<int>(box.left)-gap-(static_cast<int>(client.left)+gap);
    double fit=1;
    if (beside) fit=std::clamp(double((std::max)(right_room,left_room)-pad*2)/m.S(MovePreview::kWidth),0.5,1.0);
    const int w=static_cast<int>(m.S(MovePreview::kWidth)*fit),h=static_cast<int>(m.S(MovePreview::kHeight)*fit);
    const int cw=w+pad*2,ch=h+pad*2+caption;
    const RECT& a=preview_.anchor;
    int left=(a.left+a.right)/2-cw/2; left=(std::max)(static_cast<int>(client.left)+gap,(std::min)(left,static_cast<int>(client.right)-cw-gap));
    int top=a.bottom+gap; if (top+ch>client.bottom-gap) top=(std::max)(static_cast<int>(client.top)+gap,static_cast<int>(a.top)-gap-ch);
    if (beside) {
        left=right_room>=cw||right_room>=left_room?static_cast<int>(box.right)+gap:static_cast<int>(box.left)-gap-cw;
        left=(std::max)(static_cast<int>(client.left)+gap,(std::min)(left,static_cast<int>(client.right)-cw-gap));
        top=(a.top+a.bottom)/2-ch/2;
        top=(std::max)(static_cast<int>(client.top)+gap,(std::min)(top,static_cast<int>(client.bottom)-ch-gap));
    }
    const RECT card{left,top,left+cw,top+ch};
    Rounded(graphics,card,static_cast<float>(m.S(10)),UiTheme::Base(),250,UiTheme::Mix(UiTheme::Border(),UiTheme::Accent(),35),255,1.5f);
    const RECT image{card.left+pad,card.top+pad,card.left+pad+w,card.top+pad+h};
    preview_image_=image;
    const bool picture=preview_frame_.pixels.size()==static_cast<size_t>(MovePreview::kWidth)*MovePreview::kHeight;
    if (preview_frame_.live) {
        // The game draws the stage here itself, under the hub's hole (SyncLiveView): only a dark fill until the hole is cut.
        Rounded(graphics,image,1.0f,RGB(16,16,18),255,RGB(16,16,18),0);
    } else if (picture) {
        Gdiplus::Bitmap bitmap(MovePreview::kWidth,MovePreview::kHeight,MovePreview::kWidth*4,PixelFormat32bppRGB,reinterpret_cast<BYTE*>(preview_frame_.pixels.data()));
        graphics.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBilinear);
        graphics.DrawImage(&bitmap,static_cast<INT>(image.left),static_cast<INT>(image.top),static_cast<INT>(w),static_cast<INT>(h));
    } else {
        Rounded(graphics,image,static_cast<float>(m.S(6)),UiTheme::Mix(UiTheme::Base(),RGB(128,128,128),35),255,UiTheme::Border(),0);
        Label(dc,preview_frame_.status.empty()?Tr(L"正在载入招式…"):Tr(preview_frame_.status.c_str()),image,Font(UiTheme::Font::Small),UiTheme::MutedText(),DT_SINGLELINE|DT_VCENTER|DT_CENTER);
    }
    RECT name{card.left+pad,image.bottom,card.right-pad,card.bottom};
    std::wstring pace;
    if (preview_.curve.size()>=2) pace=Tr(L"曲线")+L" "+SpeedText(static_cast<int>(std::lround(NpcCombo::CurveMeanSpeed(preview_.curve))));
    else if (preview_.speed!=100) pace=SpeedText(preview_.speed);
    if (preview_.trim_from>0||preview_.trim_to>0) {  // 裁剪: the part that plays
        wchar_t part[64]{};
        if (preview_.trim_to>0) _snwprintf_s(part,_countof(part),_TRUNCATE,Tr(L"裁剪 %.2f–%.2f s").c_str(),preview_.trim_from/1000.0,preview_.trim_to/1000.0);
        else _snwprintf_s(part,_countof(part),_TRUNCATE,Tr(L"裁剪 %.2f s 起").c_str(),preview_.trim_from/1000.0);
        pace=pace.empty()?std::wstring(part):std::wstring(part)+L" · "+pace;
    }
    if (!pace.empty()) {
        const SIZE size=Extent(dc,Font(UiTheme::Font::Small),pace);
        RECT right=name; right.left=right.right-size.cx; name.right=right.left-m.S(8);
        Label(dc,pace,right,Font(UiTheme::Font::Small),UiTheme::Accent(),DT_SINGLELINE|DT_VCENTER|DT_RIGHT);
    }
    std::wstring title=MoveText(preview_.ability);
    if (preview_.skin==kEffectSkin) {
        const NpcBossEffect* e=NpcBossEffects::Find(preview_.ability);
        title=Tr(L"特效预览")+L" · "+(e?FxFolderName(e->folder)+L" / "+[&]{ const std::string n=NpcBossEffects::ShortName(*e); return std::wstring(n.begin(),n.end()); }():Tr(L"（无效特效）"));
    }
    if (preview_.skin==kGrabSkin) {
        Node n; n.ability=preview_.ability.substr(std::strlen(kGrabPrefix));
        const MotianGrab::Variant* v=NodeGrab(n);
        title=Tr(L"投技")+L" · "+(v?v->name+L"（"+v->boss_name+L"）":Tr(L"（无效招式）"));
    }
    Label(dc,title,name,Font(UiTheme::Font::Small),UiTheme::Text(),DT_SINGLELINE|DT_VCENTER|DT_LEFT|DT_END_ELLIPSIS);
    preview_rect_=card;
}
double Ms(const LARGE_INTEGER& a,const LARGE_INTEGER& b) { LARGE_INTEGER hz{}; QueryPerformanceFrequency(&hz); return (b.QuadPart-a.QuadPart)*1000.0/hz.QuadPart; }
// combo scratch / combo zoom (see scratch_lock_), on the page's thread. "big <n>": n moves in a row under the key, a
// 加速 on every 10th, laid out by 整理; its layout and hit test times go to the log.
void ApplyScratch() {
    AcquireSRWLockExclusive(&scratch_lock_);
    const bool asked=scratch_asked_; const std::string ask=scratch_ask_; const int zoom=zoom_ask_; scratch_asked_=false; zoom_ask_=0;
    ReleaseSRWLockExclusive(&scratch_lock_);
    if (asked&&HasSkin()) {
        CloseCurve("a test chain shown"); Select(-1); drag_={}; CloseSearch(); edit_={}; picked_.clear();
        if (ask=="off") {
            scratch_=false; config_=NpcCombo::Load(moves_.profile); seen_generation_=NpcCombo::Generation();
            Logf("COMBO_SCRATCH off: the look's chain again");
        } else {
            if (ask.rfind("big ",0)==0) {
                const int moves=std::clamp(std::atoi(ask.c_str()+4),1,static_cast<int>(NpcCombo::kMaxNodes)-200);
                NpcCombo::Config big=NpcCombo::Decode(L"E1");
                int last=NpcCombo::KeyTrigger(big);
                for (int i=0;i<moves&&last>=0;++i) {
                    Node n; n.type=NodeType::Skill; n.ability=moves_.skills.empty()?"GA_Test":moves_.skills[static_cast<size_t>(i)%moves_.skills.size()];
                    const int added=NpcCombo::InsertAfter(big,last,n); if (added<0) break;
                    last=added;
                    if (i%10==0) { big.chain.push_back(NpcCombo::ModifierNode(NodeType::Speed,0,0)); NpcCombo::LinkModifier(big,added,static_cast<int>(big.chain.size())-1); }
                }
                big.placed=false;
                config_=big;
            } else config_=NpcCombo::Decode(std::wstring(ask.begin(),ask.end()));
            scratch_=true;
            Logf("COMBO_SCRATCH on: %zu nodes, in memory only (never saved)",config_.chain.size());
        }
        Layout();
        if (canvas_.right>canvas_.left) {
            LARGE_INTEGER t0{},t1{},t2{},t3{},t4{};
            QueryPerformanceCounter(&t0); if (!config_.placed) Arrange(); QueryPerformanceCounter(&t1);
            Frame(false); framed_=true;
            const HDC dc=GetDC(window_);
            const auto shapes=[&]{ size_t sum=0; for (size_t i=0;i<config_.chain.size();++i) sum+=static_cast<size_t>(NodeShape(dc,i).rect.right); return sum; };
            DropCanvasCache(); widths_.clear();
            QueryPerformanceCounter(&t2); size_t sum=shapes(); QueryPerformanceCounter(&t3);
            DropCanvasCache(); sum+=shapes(); QueryPerformanceCounter(&t4);
            LARGE_INTEGER t5{}; sum+=shapes(); QueryPerformanceCounter(&t5);
            ReleaseDC(window_,dc);
            Logf("COMBO_SCRATCH layout %.1f ms; every card's shape: %.1f ms measuring all text, %.1f ms re-measured (widths kept), %.2f ms with the sizes kept (%zu)",
                 Ms(t0,t1),Ms(t2,t3),Ms(t3,t4),Ms(t4,t5),sum%10);
        }
        Invalidate();
    }
    if (zoom<0&&HasSkin()) { Frame(); Logf("COMBO_SCRATCH frame all: zoom %d%%",static_cast<int>(std::lround(Zoom()*100))); }
    else if (zoom>0) {
        for (int i=0;i<static_cast<int>(std::size(kZooms));++i) if (std::lround(kZooms[i]*100)==zoom) zoom_index_=i;
        Logf("COMBO_SCRATCH zoom %d%%",static_cast<int>(std::lround(Zoom()*100))); Invalidate();
    }
}

// ---- 派生显示: the chain going on, in the game's top-right corner ---------------------------------------------------
// A click-through layered window (as SkillToast) over the game client, drawn on the UI worker from what the game
// thread publishes (NpcCombo::ChainState) and the chain's saved set (NpcCombo::Load). 详细: the chain from its start
// as a thumbnail of the page's canvas (its nodes where the page has them), the move playing lit, the node the chain
// waits at outlined, the move a kept press plays next dashed and the moves the next press may play marked. 简略: the
// move playing, then a row per press that takes the chain on (its keys as the page shows them and the move it
// plays), the kept press's row lit. Both show how long the chain still waits for a press. It stays up while
// NpcCombo::ChainHud is on and the game is in front, hidden only while the hub covers the game: with no chain going
// on it shows the chain it showed last (else the profile's first start) waiting at its start (待命), and while the game
// thread publishes nothing new (a load, a pause) the last state it had.
constexpr wchar_t kHudClass[]=L"PBZChainHud_v1";
constexpr double kHudRight=24,kHudTop=24,kHudShadow=14,kHudPad=12,kHudMapW=360,kHudMapH=230,kHudMapScale=.30,kHudSimpleW=300,kHudSimpleMaxW=460;
constexpr ULONGLONG kHudStaleMs=1000;
constexpr size_t kHudRowsMax=12;  // rows of presses the overlay lists at most
struct HudState {
    HWND window=nullptr; UiSurface::Surface surface; double scale=1; int width=0,height=0,margin=0,last_alpha=-1; POINT last{-1,-1};
    NpcCombo::ChainState state; ULONGLONG wait_from=0,drawn_at=0; int mode=0,drawn_mode=-1,last_trigger=-1; bool shown=false,idle=false,drawn_idle=false;
    std::string profile; uint64_t generation=0; NpcCombo::Config config; NpcCombo::ChainState drawn; uint64_t drawn_generation=0; UINT drawn_dpi=0;
    std::vector<bool> drawn_picks;
    UiMotion::Tween fade;
} hud_;
int HS(double v) { return static_cast<int>(std::lround(v*hud_.scale)); }
UINT HudDpi() { return static_cast<UINT>(std::lround(96*hud_.scale)); }
const Node* HudNode(const NpcCombo::Config& c,int i) { return i>=0&&static_cast<size_t>(i)<c.chain.size()?&c.chain[static_cast<size_t>(i)]:nullptr; }
// What the overlay calls a node: a move's name (as the page's fields), else the card's title.
std::wstring HudName(const NpcCombo::Config& c,int i) {
    const Node* n=HudNode(c,i); if (!n) return {};
    switch (n->type) {
    case NodeType::Skill: case NodeType::BossSkill: case NodeType::Assist: return MoveText(n->ability);
    case NodeType::WeaponMove: return WeaponMoveText(n->weapon,n->ability);
    case NodeType::Grab: return GrabField(*n);
    case NodeType::Look: return Tr(L"变身 · ")+BossLookName(n->boss);
    case NodeType::Group: {
        wchar_t text[64]{}; _snwprintf_s(text,_countof(text),_TRUNCATE,Tr(L"组合 · %zu 招").c_str(),NpcCombo::GroupSteps(c,i).size()); return text;
    }
    default: return NodeTitle(*n);
    }
}
// The chain's start: its button (a key's own keys), or the 武器派生修改.
std::wstring HudStart(const NpcCombo::Config& c,int t) {
    const Node* n=HudNode(c,t); if (!n) return {};
    if (n->type==NodeType::WeaponActions) return Tr(n->air_set?L"空中派生":L"武器派生修改");
    if (n->trigger==static_cast<int>(Trigger::NpcKey)) return Tr(L"NPC 技能键");
    if (NpcCombo::OwnKeysTrigger(n->trigger)) return KeysText(*n);
    return ButtonLabel(n->trigger);
}
// A next press's keys from x at height h (draw false: measured only); returns the width.
int HudKeys(HDC dc,const NpcCombo::Config& c,int trigger,const NpcCombo::NextInput& in,int x,int y,int h,bool draw,COLORREF muted) {
    std::vector<int> keys; bool chord=false,raw=false;
    if (in.button==NpcCombo::kThenKeyButton) {
        const Node* t=HudNode(c,trigger); const int key=t?NpcCombo::ThenRawKey(*t):-1;
        if (key<0) return 0;
        const int w=KeyIconWidth(dc,key,true,h); if (draw) DrawKeyIcon(dc,x,y,key,true,h); return w;
    }
    if (NpcCombo::OwnKeysTrigger(in.button)) {
        const Node* b=HudNode(c,in.branch);
        if (b&&in.row>=0&&static_cast<size_t>(in.row)<b->routes.size()) { const auto& r=b->routes[static_cast<size_t>(in.row)]; keys=r.keys; chord=r.chord; raw=RawRoute(r); }
        else if (const Node* t=HudNode(c,trigger)) { keys=t->keys; chord=t->chord; raw=RawTrigger(*t); }
        int width=0;
        for (size_t k=0;k<keys.size();++k) {
            if (k) {
                const int sep=KeySeparator(h);
                if (draw) { RECT between{x+width,y,x+width+sep,y+h}; SetTextColor(dc,muted); DrawTextW(dc,chord?L"+":L"→",-1,&between,DT_SINGLELINE|DT_VCENTER|DT_CENTER|DT_NOPREFIX); }
                width+=sep;
            }
            const int w=KeyIconWidth(dc,keys[k],raw,h); if (draw) DrawKeyIcon(dc,x+width,y,keys[k],raw,h); width+=w;
        }
        return width;
    }
    if (in.button==static_cast<int>(Trigger::NpcKey)) {
        const UiHotkeys::Chord key=UiHotkeys::Get(UiHotkeys::Action::NpcSkill);
        if (key.empty()) { const std::wstring t=Tr(L"NPC 技能键"); const int w=UiHotkeys::KeyWidth(dc,h,t.c_str()); if (draw) UiHotkeys::DrawKey(dc,x,y,h,t.c_str()); return w; }
        const int w=(std::max)(h,UiHotkeys::Width(dc,h,key)-h/5); if (draw) UiHotkeys::Draw(dc,x,y,h,key,x+w+h); return w;
    }
    const std::wstring label=ButtonLabel(in.button);
    const int w=UiHotkeys::KeyWidth(dc,h,label.c_str()); if (draw) UiHotkeys::DrawKey(dc,x,y,h,label.c_str()); return w;
}
int HudTextWidth(HDC dc,HFONT font,const std::wstring& text) {
    const HGDIOBJ old=SelectObject(dc,font); SIZE size{}; GetTextExtentPoint32W(dc,text.c_str(),static_cast<int>(text.size()),&size); SelectObject(dc,old); return size.cx;
}
void HudText(HDC dc,HFONT font,const std::wstring& text,RECT rect,COLORREF color,UINT flags) {
    const HGDIOBJ old=SelectObject(dc,font); SetTextColor(dc,color); SetBkMode(dc,TRANSPARENT);
    DrawTextW(dc,text.c_str(),-1,&rect,flags|DT_NOPREFIX); SelectObject(dc,old);
}
// The canvas box a node takes in the thumbnail (canvas units, about the page's card).
RECT HudBox(const Node& n) {
    int w=300,h=kHeader+kRow*2+kPad;
    switch (n.type) {
    case NodeType::Group: w=NpcCombo::ClampGroupW(n.w); h=NpcCombo::ClampGroupH(n.h); break;
    case NodeType::Branch: w=500; h=kHeader+kRow*(static_cast<int>(n.routes.size())+2)+kPad; break;
    case NodeType::WeaponActions: w=500; h=kHeader+kRow*(static_cast<int>(n.routes.size())+5)+kPad; break;
    case NodeType::Trigger: h=kHeader+kRow*5+kPad; break;
    case NodeType::Interval: w=200; break;
    case NodeType::BossSkill: case NodeType::Hook: case NodeType::WeaponMove: case NodeType::Teleport: case NodeType::Assist: w=340; h=kHeader+kRow*5+kPad; break;
    default: break;
    }
    return {n.x,n.y,n.x+w,n.y+h};
}
// The wait bar: how much of the chain's wait for a press is left (0..1; -1: not waiting).
double HudWaitLeft(ULONGLONG now) {
    const ULONGLONG until=hud_.state.wait_until;
    if (!until||now>=until||hud_.wait_from>=until) return -1;
    return static_cast<double>(until-now)/static_cast<double>(until-hud_.wait_from);
}
void HudBar(HDC dc,Gdiplus::Graphics& g,const RECT& track,double left) {
    if (left<0) return;
    Rounded(g,track,static_cast<float>(HS(2)),UiTheme::Mix(UiTheme::Panel(),UiTheme::Text(),14),255,0,0);
    RECT fill=track; fill.right=fill.left+static_cast<int>(std::lround((track.right-track.left)*std::clamp(left,0.0,1.0)));
    if (fill.right>fill.left+1) Rounded(g,fill,static_cast<float>(HS(2)),left<.25?UiTheme::Mix(UiTheme::Accent(),RGB(230,80,60),60):UiTheme::Accent(),255,0,0);
    (void)dc;
}
bool HudStartNode(const NpcCombo::Config& c,int i);
// Where the chain has rows that one press chooses between (the same button: a tap or 按住, now or after a 停顿), the
// row that press takes now, as the game thread decides it (NpcCombo::Follow, then Decide's shortest pause): the rows
// whose pause is over since the branch's pause start and whose hold the button held now has reached; of those the
// longest hold, then the longest pause. Too early for all: the row of the shortest pause still to come (the press is
// kept for it). picks[k] for next[k]; a press with one row only is not picked.
std::vector<bool> HudPicks(const NpcCombo::ChainState& s,const std::vector<NpcCombo::NextInput>& next,ULONGLONG now) {
    std::vector<bool> picks(next.size(),false);
    const long long since=s.branch_at&&now>=s.branch_at?static_cast<long long>(now-s.branch_at):-1;
    const auto own_keys=[](const NpcCombo::NextInput& in){ return NpcCombo::OwnKeysTrigger(in.button); };
    const auto same_press=[&](const NpcCombo::NextInput& a,const NpcCombo::NextInput& b){
        if (own_keys(a)||own_keys(b)) return a.branch==b.branch&&a.row==b.row;
        return a.button==b.button;
    };
    const auto held_for=[&](const NpcCombo::NextInput& in)->long long{
        if (!s.hold_down||now<s.hold_down) return 0;
        const NpcCombo::Press& p=s.hold_press;
        const bool match=own_keys(in)?p.branch==in.branch&&p.row==in.row:NpcCombo::Matches(in.button,p);
        return match?static_cast<long long>(now-s.hold_down):0;
    };
    std::vector<bool> done(next.size(),false);
    for (size_t a=0;a<next.size();++a) {
        if (done[a]) continue;
        std::vector<size_t> group;
        for (size_t b=a;b<next.size();++b) if (!done[b]&&same_press(next[a],next[b])) { group.push_back(b); done[b]=true; }
        if (group.size()<2) continue;
        const long long held=held_for(next[a]);
        int best=-1,later=-1;
        for (const size_t k:group) {
            const NpcCombo::NextInput& in=next[k];
            if (in.hold_ms>held) continue;
            if (in.pause_ms>0&&(since<0||since<in.pause_ms)) {
                if (later<0||in.pause_ms<next[static_cast<size_t>(later)].pause_ms) later=static_cast<int>(k);
                continue;
            }
            if (best<0) { best=static_cast<int>(k); continue; }
            const NpcCombo::NextInput& was=next[static_cast<size_t>(best)];
            if (in.hold_ms>was.hold_ms||(in.hold_ms==was.hold_ms&&in.pause_ms>was.pause_ms)) best=static_cast<int>(k);
        }
        const int pick=best>=0?best:later;
        if (pick>=0) picks[static_cast<size_t>(pick)]=true;
    }
    return picks;
}
// Paints the overlay into hud_.surface (body inside a shadow margin); false: nothing to show.
bool HudRender(ULONGLONG now) {
    const NpcCombo::Config& c=hud_.config; const NpcCombo::ChainState& s=hud_.state;
    const UINT dpi=HudDpi();
    const HFONT small_font=UiTheme::GetFont(UiTheme::Font::Small,dpi),strong=UiTheme::GetFont(UiTheme::Font::Strong,dpi),body_font=UiTheme::GetFont(UiTheme::Font::Body,dpi);
    const COLORREF text=UiTheme::Text(),muted=UiTheme::MutedText(),accent=UiTheme::Accent(),panel=UiTheme::Mix(UiTheme::Panel(),UiTheme::Text(),4);
    const int pad=HS(kHudPad),current=s.playing>=0?s.playing:s.cursor;
    hud_.margin=HS(kHudShadow);
    if (!HudNode(c,s.trigger)) {  // a profile with no start: the title and a line saying so
        const std::wstring title=Tr(L"派生显示"),note=Tr(L"还没有派生链");
        HDC measure=CreateCompatibleDC(nullptr);
        hud_.width=(std::max)({HudTextWidth(measure,strong,title),HudTextWidth(measure,small_font,note),HS(200)})+2*pad;
        DeleteDC(measure);
        hud_.height=pad+HS(22)+HS(4)+HS(18)+pad;
        if (!hud_.surface.Ensure(hud_.width+2*hud_.margin,hud_.height+2*hud_.margin)) return false;
        hud_.surface.Clear(); HDC dc=hud_.surface.dc; SetBkMode(dc,TRANSPARENT);
        const RECT body{hud_.margin,hud_.margin,hud_.margin+hud_.width,hud_.margin+hud_.height};
        SetDCBrushColor(dc,panel); FillRect(dc,&body,static_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
        int y=body.top+pad;
        HudText(dc,strong,title,{body.left+pad,y,body.right-pad,y+HS(22)},text,DT_SINGLELINE|DT_VCENTER|DT_LEFT|DT_END_ELLIPSIS); y+=HS(22)+HS(4);
        HudText(dc,small_font,note,{body.left+pad,y,body.right-pad,y+HS(18)},muted,DT_SINGLELINE|DT_VCENTER|DT_LEFT|DT_END_ELLIPSIS);
        hud_.surface.Shape(body,HS(10),UiTheme::Mix(UiTheme::Border(),accent,30),hud_.margin,.45);
        return true;
    }
    const std::wstring start=HudStart(c,s.trigger),now_name=hud_.idle?Tr(L"待命"):current==s.trigger?Tr(L"派生开始"):HudName(c,current);
    const std::vector<NpcCombo::NextInput> next=NpcCombo::NextInputs(c,s.cursor,s.button,kHudRowsMax);
    const std::vector<bool> picks=hud_.idle?std::vector<bool>(next.size(),false):HudPicks(s,next,now);
    // Every press that does something now: the chain's next presses (none at 待命), then under 其他起手 the first press
    // of every other start (it starts that chain over this one); at 待命 every start's. At most kHudRowsMax rows.
    struct HudRow { int trigger=-1; NpcCombo::NextInput in; bool other=false; };
    std::vector<HudRow> rows; size_t more=0;
    const auto push=[&](int t,const NpcCombo::NextInput& in,bool other){ if (rows.size()<kHudRowsMax) rows.push_back({t,in,other}); else ++more; };
    if (!hud_.idle) for (const auto& in:next) push(s.trigger,in,false);
    const size_t own=rows.size();
    for (size_t i=0;i<c.chain.size();++i) {
        const int t=static_cast<int>(i);
        if (!HudStartNode(c,t)||(!hud_.idle&&t==s.trigger)) continue;
        const Node& n=c.chain[i];
        for (const auto& in:NpcCombo::NextInputs(c,t,n.type==NodeType::Trigger?n.trigger:0,4)) push(t,in,!hud_.idle);
    }
    const double wait=HudWaitLeft(now);
    // A row's tags: its 按住 and its pause.
    const auto tags_of=[](const NpcCombo::NextInput& in){
        std::wstring tags;
        if (in.hold_ms>0) { wchar_t t[32]{}; _snwprintf_s(t,_countof(t),_TRUNCATE,Tr(L"按住 %.1f s").c_str(),in.hold_ms/1000.0); tags=t; }
        if (in.pause_ms>0) { wchar_t t[32]{}; _snwprintf_s(t,_countof(t),_TRUNCATE,Tr(L"停顿 %.1f s").c_str(),in.pause_ms/1000.0); tags+=(tags.empty()?L"":L" · ")+std::wstring(t); }
        return tags;
    };
    const int sec_h=HS(18);
    const auto measure_rows=[&](HDC dc,int key_h,HFONT name_font,int& keys_w,int& names_w){
        keys_w=0; names_w=0;
        for (const HudRow& r:rows) {
            keys_w=(std::max)(keys_w,HudKeys(dc,c,r.trigger,r.in,0,0,key_h,false,muted));
            const std::wstring tags=tags_of(r.in);
            names_w=(std::max)(names_w,HudTextWidth(dc,name_font,HudName(c,r.in.move))+(tags.empty()?0:HudTextWidth(dc,small_font,tags)+HS(14)));
        }
    };
    const auto rows_height=[&](int row_h){
        int h=static_cast<int>(rows.size())*row_h;
        if (!hud_.idle&&own==0) h+=row_h;                        // 派生到此结束
        if (own<rows.size()&&!hud_.idle) h+=sec_h;               // 其他起手
        if (more) h+=sec_h;
        return h;
    };
    // The rows from y: a key column keys_w wide, then the move (the kept press's row lit), its tags on the right.
    const auto draw_rows=[&](HDC dc,Gdiplus::Graphics& g,const RECT& body,int& y,int key_h,int row_h,HFONT name_font,int keys_w){
        if (!hud_.idle&&own==0) { HudText(dc,small_font,Tr(L"派生到此结束"),{body.left+pad,y,body.right-pad,y+row_h},muted,DT_SINGLELINE|DT_VCENTER|DT_LEFT); y+=row_h; }
        for (size_t k=0;k<rows.size();++k) {
            const HudRow& r=rows[k]; const NpcCombo::NextInput& in=r.in;
            if (r.other&&(k==0||!rows[k-1].other)) { HudText(dc,small_font,Tr(L"其他起手"),{body.left+pad,y,body.right-pad,y+sec_h},muted,DT_SINGLELINE|DT_BOTTOM|DT_LEFT); y+=sec_h; }
            const RECT row{body.left+pad-HS(6),y+HS(1),body.right-pad+HS(6),y+row_h-HS(1)};
            const bool kept=!r.other&&in.move==s.queued,pick=!r.other&&k<own&&k<picks.size()&&picks[k];
            // The kept press's row filled; the row a press now takes (of rows one press chooses between) outlined.
            if (kept) Rounded(g,row,static_cast<float>(HS(6)),UiTheme::Mix(panel,accent,30),255,accent,200,1.f);
            else if (pick) Rounded(g,row,static_cast<float>(HS(6)),UiTheme::Mix(panel,accent,10),255,accent,230,1.5f);
            const int x=body.left+pad;
            HudKeys(dc,c,r.trigger,in,x,y+(row_h-key_h)/2,key_h,true,muted);
            const std::wstring tags=tags_of(in);
            const int tags_w=tags.empty()?0:HudTextWidth(dc,small_font,tags)+HS(8);
            // The name gets the room the tags leave; past the widest the panel takes, the tags give way first.
            const int name_left=x+keys_w+HS(12),name_need=HudTextWidth(dc,name_font,HudName(c,in.move));
            const int tag_room=(std::max)(0,static_cast<int>(body.right-pad-name_left)-(std::min)(name_need,HS(90)));
            const int tags_shown=(std::min)(tags_w,tag_room);
            const RECT name{name_left,y,body.right-pad-tags_shown,y+row_h};
            HudText(dc,name_font,HudName(c,in.move),name,kept||pick?text:UiTheme::Mix(text,muted,r.other?45:25),DT_SINGLELINE|DT_VCENTER|DT_LEFT|DT_END_ELLIPSIS);
            if (tags_shown>0) HudText(dc,small_font,tags,{body.right-pad-tags_shown,y,body.right-pad,y+row_h},pick?accent:muted,DT_SINGLELINE|DT_VCENTER|DT_RIGHT|DT_END_ELLIPSIS);
            y+=row_h;
        }
        if (more) {
            wchar_t t[48]{}; _snwprintf_s(t,_countof(t),_TRUNCATE,Tr(L"还有 %zu 个按键").c_str(),more);
            HudText(dc,small_font,t,{body.left+pad,y,body.right-pad,y+sec_h},muted,DT_SINGLELINE|DT_VCENTER|DT_LEFT); y+=sec_h;
        }
    };
    HDC measure=CreateCompatibleDC(nullptr);
    if (hud_.mode==NpcCombo::kHudSimple) {
        // 简略: the start (small), the move playing (strong), a row per press that does something now, the wait bar.
        const int key_h=HS(20),row_h=HS(28);
        int keys_w=0,names_w=0; measure_rows(measure,key_h,body_font,keys_w,names_w);
        const std::wstring head=hud_.idle?Tr(L"全部起手"):start;  // 待命 lists every start
        const int title_w=(std::max)(HudTextWidth(measure,strong,now_name),HudTextWidth(measure,small_font,head));
        hud_.width=std::clamp((std::max)(title_w,keys_w+HS(12)+names_w)+2*pad,HS(kHudSimpleW),HS(kHudSimpleMaxW));
        hud_.height=pad+HS(16)+HS(24)+HS(8)+rows_height(row_h)+(wait>=0?HS(10):0)+pad;
        DeleteDC(measure);
        if (!hud_.surface.Ensure(hud_.width+2*hud_.margin,hud_.height+2*hud_.margin)) return false;
        hud_.surface.Clear(); HDC dc=hud_.surface.dc; SetBkMode(dc,TRANSPARENT);
        const RECT body{hud_.margin,hud_.margin,hud_.margin+hud_.width,hud_.margin+hud_.height};
        SetDCBrushColor(dc,panel); FillRect(dc,&body,static_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
        Gdiplus::Graphics g(dc); g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        int y=body.top+pad;
        HudText(dc,small_font,head,{body.left+pad,y,body.right-pad,y+HS(16)},muted,DT_SINGLELINE|DT_VCENTER|DT_LEFT|DT_END_ELLIPSIS); y+=HS(16);
        const RECT bar{body.left+pad,y+HS(4),body.left+pad+HS(4),y+HS(20)};
        Rounded(g,bar,static_cast<float>(HS(2)),accent,255,0,0);
        HudText(dc,strong,now_name,{bar.right+HS(8),y,body.right-pad,y+HS(24)},text,DT_SINGLELINE|DT_VCENTER|DT_LEFT|DT_END_ELLIPSIS); y+=HS(24)+HS(8);
        draw_rows(dc,g,body,y,key_h,row_h,body_font,keys_w);
        if (wait>=0) HudBar(dc,g,{body.left+pad,y+HS(5),body.right-pad,y+HS(8)},wait);
        hud_.surface.Shape(body,HS(10),UiTheme::Mix(UiTheme::Border(),accent,30),hud_.margin,.45);
        return true;
    }
    // 详细: the nodes the start's wires reach (and those in the 组合 among them), as the page places them.
    std::vector<char> in(c.chain.size(),0);
    in[static_cast<size_t>(s.trigger)]=1;
    for (const size_t i:NpcCombo::Order(c,s.trigger)) in[i]=1;
    for (bool grew=true;grew;) {
        grew=false;
        for (size_t i=0;i<c.chain.size();++i) {
            const Node& n=c.chain[i];
            if (in[i]||n.group<0||static_cast<size_t>(n.group)>=c.chain.size()||!in[static_cast<size_t>(n.group)]||NpcCombo::IsModifier(n)||NpcCombo::IsSetting(n)) continue;
            in[i]=1; grew=true;
        }
    }
    LONG x0=LONG_MAX,y0=LONG_MAX,x1=LONG_MIN,y1=LONG_MIN;
    for (size_t i=0;i<c.chain.size();++i) if (in[i]) { const RECT b=HudBox(c.chain[i]); x0=(std::min)(x0,b.left); y0=(std::min)(y0,b.top); x1=(std::max)(x1,b.right); y1=(std::max)(y1,b.bottom); }
    const double bw=(std::max)(1L,x1-x0),bh=(std::max)(1L,y1-y0);
    const double k=(std::min)({HS(kHudMapW)/bw,HS(kHudMapH)/bh,kHudMapScale*hud_.scale});
    const int map_w=static_cast<int>(std::lround(bw*k)),map_h=static_cast<int>(std::lround(bh*k));
    const std::wstring status=hud_.idle?Tr(L"待命 · 按起手键开始"):s.playing>=0?Tr(L"正在：")+now_name:s.queued>=0?Tr(L"接下来：")+HudName(c,s.queued):wait>=0?Tr(L"等待下一键"):now_name;
    const int head_w=HudTextWidth(measure,strong,start)+HS(16),foot_w=HudTextWidth(measure,small_font,status);
    // Under the map, the presses that do something now (as 简略's, smaller).
    const int list_key_h=HS(16),list_row_h=HS(22);
    int list_keys_w=0,list_names_w=0; measure_rows(measure,list_key_h,small_font,list_keys_w,list_names_w);
    DeleteDC(measure);
    hud_.width=(std::max)({map_w,head_w,foot_w,HS(220),(std::min)(list_keys_w+HS(12)+list_names_w,HS(kHudSimpleMaxW))})+2*pad;
    hud_.height=pad+HS(22)+HS(6)+map_h+HS(8)+HS(18)+(wait>=0?HS(9):0)+HS(6)+rows_height(list_row_h)+pad;
    if (!hud_.surface.Ensure(hud_.width+2*hud_.margin,hud_.height+2*hud_.margin)) return false;
    hud_.surface.Clear(); HDC dc=hud_.surface.dc; SetBkMode(dc,TRANSPARENT);
    const RECT body{hud_.margin,hud_.margin,hud_.margin+hud_.width,hud_.margin+hud_.height};
    SetDCBrushColor(dc,panel); FillRect(dc,&body,static_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
    Gdiplus::Graphics g(dc); g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    int y=body.top+pad;
    HudText(dc,strong,start,{body.left+pad,y,body.right-pad,y+HS(22)},text,DT_SINGLELINE|DT_VCENTER|DT_LEFT|DT_END_ELLIPSIS); y+=HS(22)+HS(6);
    const int ox=body.left+pad+((body.right-body.left-2*pad)-map_w)/2,oy=y;
    const auto at=[&](LONG cx,LONG cy){ return POINT{ox+static_cast<int>(std::lround((cx-x0)*k)),oy+static_cast<int>(std::lround((cy-y0)*k))}; };
    const auto box=[&](size_t i){ const RECT b=HudBox(c.chain[i]); const POINT a=at(b.left,b.top),z=at(b.right,b.bottom); return RECT{a.x,a.y,(std::max)(a.x+HS(6),z.x),(std::max)(a.y+HS(4),z.y)}; };
    std::vector<char> lead(c.chain.size(),0);  // the moves the next press may play
    for (const auto& n:next) if (n.move>=0&&static_cast<size_t>(n.move)<lead.size()) lead[static_cast<size_t>(n.move)]=1;
    // 组合 frames under everything.
    for (size_t i=0;i<c.chain.size();++i) if (in[i]&&c.chain[i].type==NodeType::Group) {
        const bool lit=s.cursor==static_cast<int>(i);
        Rounded(g,box(i),static_cast<float>(HS(4)),UiTheme::Mix(panel,GroupColor(),14),255,lit?accent:UiTheme::Mix(UiTheme::Border(),GroupColor(),40),255,lit?2.f:1.f);
    }
    // Wires: from an output (a row's for a 按键分支 / 武器派生修改) to the target's header.
    for (size_t i=0;i<c.chain.size();++i) {
        if (!in[i]) continue;
        const Node& n=c.chain[i]; const RECT from=box(i);
        for (size_t o=0;o<NpcCombo::OutCount(n);++o) {
            const int to=NpcCombo::OutOf(n,o);
            if (to<0||static_cast<size_t>(to)>=c.chain.size()||!in[static_cast<size_t>(to)]) continue;
            const int row=n.type==NodeType::Branch?1+static_cast<int>(o):n.type==NodeType::WeaponActions?3+static_cast<int>(o):-1;
            const POINT a{from.right,row<0?from.top+static_cast<int>(std::lround(kHeader*k/2)):from.top+static_cast<int>(std::lround((kHeader+row*kRow+kRow/2)*k))};
            const RECT tb=box(static_cast<size_t>(to)); const POINT b{tb.left,tb.top+static_cast<int>(std::lround(kHeader*k/2))};
            const bool hot=static_cast<int>(i)==s.cursor||(lead[static_cast<size_t>(to)]&&(row>=0||static_cast<int>(i)==s.cursor));
            Gdiplus::Pen pen(Argb(hot?accent:muted,hot?230:110),hot?2.f:1.f);
            const float dx=(std::max)(12.f,std::fabs(static_cast<float>(b.x-a.x))*.45f);
            g.DrawBezier(&pen,Gdiplus::PointF(static_cast<float>(a.x),static_cast<float>(a.y)),Gdiplus::PointF(a.x+dx,static_cast<float>(a.y)),
                         Gdiplus::PointF(b.x-dx,static_cast<float>(b.y)),Gdiplus::PointF(static_cast<float>(b.x),static_cast<float>(b.y)));
        }
    }
    // Nodes: their colour; the move playing filled with the accent, the node the chain is at outlined, a kept press's
    // move dashed, the next press's moves marked. A name when the box has room.
    for (size_t i=0;i<c.chain.size();++i) {
        if (!in[i]||c.chain[i].type==NodeType::Group) continue;
        const Node& n=c.chain[i]; const RECT r=box(i); const int idx=static_cast<int>(i);
        const bool playing=idx==s.playing,at_node=idx==s.cursor&&s.playing<0,kept=idx==s.queued,leads=lead[i]!=0;
        const COLORREF hue=NodeHue(n);
        if (playing) { RECT glow=r; InflateRect(&glow,HS(3),HS(3)); Rounded(g,glow,static_cast<float>(HS(6)),accent,70,0,0); }
        Rounded(g,r,static_cast<float>(HS(3)),playing?UiTheme::Mix(accent,text,10):UiTheme::Mix(panel,hue,leads||kept?55:38),255,
                playing||at_node?accent:UiTheme::Mix(UiTheme::Border(),hue,50),255,playing||at_node?2.f:1.f);
        if (kept) {
            Gdiplus::Pen dash(Argb(accent,255),2.f); dash.SetDashStyle(Gdiplus::DashStyleDash);
            RECT d=r; InflateRect(&d,HS(2),HS(2));
            g.DrawRectangle(&dash,static_cast<float>(d.left),static_cast<float>(d.top),static_cast<float>(d.right-d.left),static_cast<float>(d.bottom-d.top));
        } else if (leads&&!playing) {
            Gdiplus::Pen mark(Argb(accent,150),1.f);
            g.DrawRectangle(&mark,static_cast<float>(r.left-HS(1)),static_cast<float>(r.top-HS(1)),static_cast<float>(r.right-r.left+HS(2)),static_cast<float>(r.bottom-r.top+HS(2)));
        }
        if (r.right-r.left>=HS(44)&&r.bottom-r.top>=HS(12)) {
            RECT t=r; InflateRect(&t,-HS(3),0);
            const std::wstring name=n.type==NodeType::Trigger||n.type==NodeType::WeaponActions?HudStart(c,idx):HudName(c,idx);
            HudText(dc,small_font,name,t,playing?UiTheme::Mix(panel,text,0):text,DT_SINGLELINE|DT_VCENTER|DT_CENTER|DT_END_ELLIPSIS);
        }
    }
    y=oy+map_h+HS(8);
    HudText(dc,small_font,status,{body.left+pad,y,body.right-pad,y+HS(18)},s.playing>=0?text:muted,DT_SINGLELINE|DT_VCENTER|DT_LEFT|DT_END_ELLIPSIS); y+=HS(18);
    if (wait>=0) { HudBar(dc,g,{body.left+pad,y+HS(3),body.right-pad,y+HS(6)},wait); y+=HS(9); }
    y+=HS(6);
    draw_rows(dc,g,body,y,list_key_h,list_row_h,small_font,list_keys_w);
    hud_.surface.Shape(body,HS(10),UiTheme::Mix(UiTheme::Border(),accent,30),hud_.margin,.45);
    return true;
}
bool HudCreate() {
    if (hud_.window) return true;
    WNDCLASSW cls{}; cls.lpfnWndProc=DefWindowProcW; cls.hInstance=GetModuleHandleW(nullptr); cls.lpszClassName=kHudClass;
    if (!RegisterClassW(&cls)&&GetLastError()!=ERROR_CLASS_ALREADY_EXISTS) return false;
    hud_.window=CreateWindowExW(WS_EX_LAYERED|WS_EX_NOACTIVATE|WS_EX_TRANSPARENT,kHudClass,L"",WS_POPUP,0,0,1,1,nullptr,nullptr,cls.hInstance,nullptr);
    if (!hud_.window) return false;
    if (!UiHost::Attach(hud_.window)) { DestroyWindow(hud_.window); hud_.window=nullptr; return false; }
    return true;
}
// The state shown and the chain it names (read again when the set changes).
void HudLoad(const std::string& profile) {
    const uint64_t generation=NpcCombo::Generation();
    if (profile!=hud_.profile) hud_.last_trigger=-1;  // the node numbers are another set's
    if (profile!=hud_.profile||generation!=hud_.generation) { hud_.config=NpcCombo::Load(profile); hud_.profile=profile; hud_.generation=generation; }
}
void HudTake(const NpcCombo::ChainState& s,ULONGLONG now) {
    if (s.wait_until!=hud_.state.wait_until||hud_.idle) hud_.wait_from=s.wait_until?now:0;
    HudLoad(s.profile);
    hud_.state=s; hud_.idle=false; hud_.last_trigger=s.trigger;
}
// A node a chain may start at: an on 按键 start or 武器派生修改.
bool HudStartNode(const NpcCombo::Config& c,int i) {
    const Node* n=HudNode(c,i); return n&&n->on&&(n->type==NodeType::Trigger||n->type==NodeType::WeaponActions);
}
// 待命: no chain going on; the chain shown last (else the set's first start), at its start.
void HudIdle(const std::string& profile,ULONGLONG now) {
    HudLoad(profile);
    const NpcCombo::Config& c=hud_.config;
    int t=HudStartNode(c,hud_.last_trigger)?hud_.last_trigger:-1;
    for (size_t i=0;t<0&&i<c.chain.size();++i) if (HudStartNode(c,static_cast<int>(i))) t=static_cast<int>(i);
    NpcCombo::ChainState s; s.profile=profile; s.trigger=t; s.cursor=t; s.at=now;
    if (const Node* n=HudNode(c,t)) s.button=n->type==NodeType::Trigger?n->trigger:0;
    hud_.state=s; hud_.wait_from=0; hud_.idle=true;
}
}  // namespace
bool HandleCommand(const char* arguments) {
    char verb[32]{},first[160]{}; int skill=0;
    const int read=sscanf_s(arguments?arguments:"","%31s %159s %d",verb,static_cast<unsigned>(sizeof(verb)),first,static_cast<unsigned>(sizeof(first)),&skill);
    if (read>=1&&!std::strcmp(verb,"menu")) { DebugHub::ShowPage(DebugHub::Page::Combo); return true; }
    if (read>=2&&!std::strcmp(verb,"handsoff")) { UiCursor::SetHandsOff(!std::strcmp(first,"on")); return true; }
    if (read>=1&&!std::strcmp(verb,"curve")) { InterlockedExchange(&test_curve_,read>=2&&!std::strcmp(first,"close")?2:1); return true; }
    if (read>=2&&!std::strcmp(verb,"scratch")) {  // combo scratch <Encode text> | big <n> | off
        std::string rest=std::strstr(arguments,"scratch")+7;
        while (!rest.empty()&&std::isspace(static_cast<unsigned char>(rest.front()))) rest.erase(rest.begin());
        while (!rest.empty()&&std::isspace(static_cast<unsigned char>(rest.back()))) rest.pop_back();
        AcquireSRWLockExclusive(&scratch_lock_); scratch_ask_=rest; scratch_asked_=true; ReleaseSRWLockExclusive(&scratch_lock_); return true;
    }
    if (read>=2&&!std::strcmp(verb,"zoom")) {  // combo zoom <percent> | all (显示全部)
        AcquireSRWLockExclusive(&scratch_lock_); zoom_ask_=!std::strcmp(first,"all")?-1:std::atoi(first); ReleaseSRWLockExclusive(&scratch_lock_); return true;
    }
    if (read<2||std::strcmp(verb,"preview")) return false;
    if (!std::strcmp(first,"off")) { test_hover_={}; return true; }
    if (!std::strcmp(first,"node")) { if (read<3) return false; test_hover_={true,-1,{},skill}; return true; }  // combo preview node <n>
    const int32_t skin=static_cast<int32_t>(std::strtol(first,nullptr,10));
    NpcRetarget::SkinMoves look;
    if (read<3||!NpcRetarget::GetSkinMoves(skin,look)||skill<1||static_cast<size_t>(skill)>look.skills.size()) return false;
    test_hover_={true,skin,look.skills[static_cast<size_t>(skill-1)]};
    return true;
}
void SetLogger(void (*log)(const char*)) { log_=log; }
// 派生显示 (UI worker): follows NpcCombo::ChainHud and the published chain state; up the whole time it is on.
void HudTick() {
    const ULONGLONG now=GetTickCount64();
    const int mode=NpcCombo::ChainHud();
    const bool want=mode!=NpcCombo::kHudOff&&UiHost::InGame()&&!DebugHub::ObscuresGameplay();
    if (!want&&!hud_.shown) return;
    if (want) {
        const NpcCombo::ChainState s=NpcCombo::GetChain();
        const bool fresh=s.at&&now<s.at+kHudStaleMs;
        hud_.mode=mode;
        if (fresh&&s.trigger>=0) HudTake(s,now);
        else if (fresh&&!s.profile.empty()) HudIdle(s.profile,now);
        else if (!hud_.idle&&hud_.state.trigger<0) HudIdle(hud_.profile.empty()?std::string(NpcCombo::kHeroProfile):hud_.profile,now);
        // else: nothing new from the game thread (a load, a pause): the state shown stays
    }
    if (!HudCreate()) return;
    const HWND game_window=UiHost::Game(); const UINT dpi=game_window?GetDpiForWindow(game_window):96;
    hud_.scale=std::clamp(static_cast<double>(dpi?dpi:96)/96.0,.75,2.5);
    const bool reduced=UiMotion::ReducedMotion();
    if (want&&!hud_.shown) { hud_.shown=true; hud_.fade.ease=UiMotion::EaseOutCubic; hud_.fade.Jump(0); hud_.drawn_mode=-1; }
    hud_.fade.Set(want?1.0:0.0,want?160:260,now,reduced);
    if (!want&&!hud_.fade.Running(now)) { ShowWindow(hud_.window,SW_HIDE); hud_.shown=false; return; }
    // Drawn again on a change, and every 50 ms while the wait bar runs.
    const NpcCombo::ChainState& d=hud_.drawn; const NpcCombo::ChainState& c=hud_.state;
    // The picked rows move with time (a pause running out, a button held long enough): drawn again when they change.
    const std::vector<bool> picks=hud_.idle?std::vector<bool>():HudPicks(c,NpcCombo::NextInputs(hud_.config,c.cursor,c.button,kHudRowsMax),now);
    const bool changed=picks!=hud_.drawn_picks||hud_.drawn_mode!=hud_.mode||hud_.drawn_dpi!=dpi||hud_.drawn_idle!=hud_.idle||hud_.drawn_generation!=hud_.generation||d.profile!=c.profile||d.trigger!=c.trigger||
        d.cursor!=c.cursor||d.playing!=c.playing||d.queued!=c.queued||d.button!=c.button||d.wait_until!=c.wait_until||
        (c.wait_until>now&&now>=hud_.drawn_at+50);
    if (changed) {
        if (!HudRender(now)) { ShowWindow(hud_.window,SW_HIDE); hud_.shown=false; return; }
        hud_.drawn=c; hud_.drawn_picks=picks; hud_.drawn_mode=hud_.mode; hud_.drawn_idle=hud_.idle; hud_.drawn_dpi=dpi; hud_.drawn_generation=hud_.generation; hud_.drawn_at=now; hud_.last={-1,-1}; hud_.last_alpha=-1;
    }
    const SIZE client=UiHost::ClientSize(); const POINT origin=UiHost::ClientOrigin();
    const POINT where{origin.x+client.cx-HS(kHudRight)-hud_.width-hud_.margin,origin.y+HS(kHudTop)-hud_.margin};
    const int alpha=static_cast<int>(std::clamp(std::lround(hud_.fade.At(now)*255),0L,255L));
    if (where.x!=hud_.last.x||where.y!=hud_.last.y||alpha!=hud_.last_alpha) { hud_.surface.Present(hud_.window,where,static_cast<BYTE>(alpha)); hud_.last=where; hud_.last_alpha=alpha; }
    if (!IsWindowVisible(hud_.window)) { ShowWindow(hud_.window,SW_SHOWNA); SetWindowPos(hud_.window,HWND_TOP,0,0,0,0,SWP_NOMOVE|SWP_NOSIZE|SWP_NOACTIVATE); }
}
void HudShutdown() {
    if (hud_.window) { UiHost::Detach(hud_.window); DestroyWindow(hud_.window); }
    hud_.window=nullptr; hud_.surface.Reset(); hud_.shown=false;
}
void Tick() {
    if (const LONG curve=InterlockedExchange(&test_curve_,0)) TestCurve(curve==2);
    BoneMotionPage::TickEditor(window_);
    SpeedCurveWindow::Tick(window_);  // closes itself when the page is hidden
    static int curve_shown=-1;  // the node whose curve window was open at the last tick (COMBO_CURVE lines)
    if (const int now=SpeedCurveWindow::EditedNode(); now!=curve_shown) {
        if (now>=0) Logf("COMBO_CURVE window open node=%d",now);
        else Logf("COMBO_CURVE window closed node=%d: %s",curve_shown,SpeedCurveWindow::LastClose());
        curve_shown=now;
    }
    if (!window_||!IsWindowVisible(window_)) { if (record_.node>=0) StopRecording(nullptr); if (preview_.asked||preview_.since) HidePreview(); return; }
    ApplyScratch();
    UpdatePreview();
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
    if (search_.open||edit_.node>=0) Repaint();  // the caret blinks
}
namespace Probe {
// 派生显示 offline: the overlay for the page's chain at that place, as 32-bit premultiplied pixels (no window).
// since_ms: how long the branch's pause has run (-1: not started).
bool HudPixels(int mode,int trigger,int cursor,int playing,int queued,unsigned long long wait_left_ms,std::vector<uint32_t>& pixels,int& width,int& height,long long since_ms) {
    const ULONGLONG now=GetTickCount64();
    hud_.config=config_; hud_.mode=mode; hud_.scale=1; hud_.idle=cursor<0;  // cursor -1: 待命 at the start
    if (hud_.idle) { cursor=trigger; playing=-1; queued=-1; wait_left_ms=0; }
    NpcCombo::ChainState s; s.profile="probe"; s.trigger=trigger; s.cursor=cursor; s.playing=playing; s.queued=queued;
    const Node* t=HudNode(config_,trigger); s.button=t&&t->type==NodeType::Trigger?NpcCombo::ThenButton(*t):static_cast<int>(Trigger::Light);
    if (cursor==trigger&&t) s.button=t->trigger;
    s.wait_until=wait_left_ms?now+wait_left_ms:0; s.at=now; s.branch_at=since_ms>=0?now-static_cast<ULONGLONG>(since_ms):0;
    hud_.state=s; hud_.wait_from=wait_left_ms?now-wait_left_ms:0;  // half the wait left
    if (!HudRender(now)) return false;
    GdiFlush(); width=hud_.surface.width; height=hud_.surface.height;
    pixels.assign(hud_.surface.pixels,hud_.surface.pixels+static_cast<size_t>(width)*height);
    return true;
}
void RefreshData(){Refresh(false);}
void PickNodes(const std::vector<int>& nodes){picked_.clear();for(int node:nodes)if(At(node))picked_.push_back(node);selected_=picked_.empty()?-1:picked_.back();}
bool ClipboardShortcut(int key){return ClipboardKey(key,true);}
size_t ClipboardCount(){return node_clipboard_.size();}
std::wstring AnimationDurationText(int index){return At(index)?AnimationDuration(static_cast<size_t>(index)):L"";}
std::wstring TrimDurationText(int index){return At(index)?TrimBarText(static_cast<size_t>(index)):L"";}
std::wstring TrimSourceLabel(int index){return At(index)?TrimSourceText(static_cast<size_t>(index)):L"";}
// Hidden-host performance fixtures only; never saved or sent to the game.
void PerformanceFixture(int count) {
    config_=NpcCombo::Decode(L"E1");config_.placed=true;scratch_=true;framed_=true;
    config_.chain.resize(static_cast<size_t>(std::clamp(count,10,10000)));
    for(size_t i=0;i<config_.chain.size();++i){auto& n=config_.chain[i];n={};n.type=NodeType::Skill;n.ability=moves_.skills.empty()?"GA_Test":moves_.skills.front();n.x=static_cast<int>(i%100)*360;n.y=static_cast<int>(i/100)*230;n.next=i+1<config_.chain.size()?static_cast<int>(i)+1:-1;}
    const auto seed=NpcCombo::Decode(L"E1");config_.chain[0]=seed.chain[static_cast<size_t>(NpcCombo::KeyTrigger(seed))];config_.chain[0].next=1;config_.chain[0].x=0;config_.chain[0].y=0;
    zoom_index_=3;pan_x_=20;pan_y_=60;selected_=-1;picked_.clear();hot_={};Layout();Invalidate();
}
void PerformanceStats(uint64_t& measures,uint64_t& graphs,uint64_t& shapes){measures=canvas_cache_.measurements;graphs=canvas_cache_.graph_builds;shapes=canvas_cache_.shape_builds;}
void PerformanceHover(POINT p){SendMessageW(window_,WM_MOUSEMOVE,0,MAKELPARAM(p.x,p.y));}
void PerformancePan(int dx){pan_x_+=static_cast<float>(dx);Repaint();}
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
POINT RouteHoldPoint(int node,int route){return Middle(ShapeOf(node).routes.at(static_cast<size_t>(route)).hvalue);}
void RoutePicker(int node,int route,int part){OpenPicker(node,route,part);}
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
HWND SearchInput(){return search_input_;}
std::wstring SearchQuery(){return search_.query;}
void SearchAt(POINT at){OpenSearch(at);}
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
int SearchSelected() { return search_.selected; }
POINT SearchRow(int row) {
    const int row_h=M().S(26),top=search_.list.top+(row-search_.scroll)*row_h;
    if (row<search_.scroll||top+row_h>search_.list.bottom||static_cast<size_t>(row)>=search_.rows.size()||search_.rows[static_cast<size_t>(row)].header) return {-1,-1};
    return {(search_.list.left+search_.list.right)/2,top+row_h/2};
}
RECT SearchBox() { return search_.open?search_.rect:RECT{}; }
RECT PreviewCard() { return preview_.asked?preview_rect_:RECT{}; }
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
POINT Toggle(int node) { return Middle(ShapeOf(node).toggle); }
void RecordAt(int node,int route){StartRecording(node,route);}
std::vector<int> RecordingKeys(){return record_.draft;}
int RecordingNode() { return record_.node; }
int RecordingRoute() { return record_.node>=0?record_.route:-1; }
void ScrollWeapons(int pixels) { library_tab_=1; Layout(); ScrollStrip(pixels>0?1:-1); }
void Library(int tab,int category) { library_tab_=std::clamp(tab,0,1); node_category_=std::clamp(category,0,4); Layout(); }
void LibraryFrom(const std::string& look) { library_boss_=look; category_=-1; library_pages_[4]=0; Layout(); }
RECT LibraryFromBox() { return library_boss_box_; }
void LibraryFromList(bool open) { if (open) OpenSourceSearch(); else if (search_.open) CloseSearch(); }
std::vector<std::string> LibraryMoves() { std::vector<std::string> out; for (const auto& p:pills_) out.push_back(p.ability); return out; }
int AddLibraryMove(size_t index) { if (index>=pills_.size()) return -1; const size_t before=config_.chain.size(); Append(LibraryNode(pills_[index].ability)); return config_.chain.size()>before?static_cast<int>(config_.chain.size()-1):-1; }
std::string NodeSummary(int index) { const Node* n=At(index); return n?std::to_string(static_cast<int>(n->type))+"|"+n->boss+"|"+n->ability:std::string(); }
RECT LibraryBox() { return library_; }
RECT LibraryTabBox(int tab) { return tab>=0&&tab<2?library_tabs_[tab]:RECT{}; }
RECT LibraryCategoryBox(int category) { return category>=0&&category<5?node_categories_[category]:RECT{}; }
void LibraryTurn(int delta) { ScrollStrip(delta); }
int LibraryPage() { return library_pages_[LibrarySection()]; }
int LibraryPageCount() { return library_page_count_; }
int ToolCategory(int button) { return NodeCategory(static_cast<Button>(button)); }
RECT ToolBox(int button) { return button>=0&&button<static_cast<int>(Button::Count)?buttons_[button]:RECT{}; }
void TrimPreviewAt(int node) { hot_={Part::NodeTrimBar,node}; UpdatePreview(); preview_.since=GetTickCount64()-kPreviewDelayMs; UpdatePreview(); }
void BonePreviewAt(int node) { hot_={Part::Node,node}; UpdatePreview(); preview_.since=GetTickCount64()-kPreviewDelayMs; UpdatePreview(); }
void BoneEditor(int node) { OpenBones(node); }
RECT TrimBarBox(int node) { return ShapeOf(node).field2; }
POINT HoldValuePoint(int node) { return Middle(ShapeOf(node).hvalue); }
bool ValueWheel(POINT p,int direction) { return WheelValue(HitTest(p),direction); }
bool TrimBegin(int node,POINT p) { return HitTest(p)==Hit{Part::NodeTrimBar,node}&&PrepareTrimDrag(node,p); }
void TrimMove(POINT p) { DragMove(p); }
bool TrimHoldToggle(int node,POINT p){return ToggleTrimHold(node,p);}
std::vector<int> TrimHoldPoints(int node){return At(node)?At(node)->trim_holds:std::vector<int>{};}
void TrimEnd(POINT p) { DragEnd(p); }
int TrimValue(int node,bool end) { return At(node)?(end?At(node)->trim_to:At(node)->trim_from):-1; }
POINT Resize(int node) { return Middle(ShapeOf(node).resize); }
// An empty point in a frame's body (left, a little under the middle).
POINT Body(int node) { const RECT r=ShapeOf(node).rect; return {r.left+(r.right-r.left)/6,r.top+(r.bottom-r.top)*11/20}; }
}
void Shutdown() {
    SpeedCurveWindow::Close();
    HidePreview();
    if (window_) DestroyWindow(window_);
    node_clipboard_.clear();paste_count_=0;
    window_=nullptr; search_input_=nullptr;pills_.clear(); tabs_.clear(); skin_=-1; pinned_=kFollowGame; game_chain_=-1; moves_={}; config_={}; search_={}; DropFonts();
}
}
