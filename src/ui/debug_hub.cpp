#include "debug_hub.h"
#include "entry_focus.h"
#include "ui_i18n.h"
#include "ui_theme.h"
#include "ui_glass.h"
#include "ui_host.h"
#include "ui_hotkeys.h"
#include "ui_cursor.h"
#include "hub_pause.h"
#include "ui_motion.h"
#include "ui_surface.h"
#include "ui_assets.h"
#include <windowsx.h>
#include <dwmapi.h>
#include <objidl.h>
#include <gdiplus.h>
#include <algorithm>
#include <array>
#include <cwchar>
#include <string>
#include <utility>
#include <cmath>

namespace DebugHub {
namespace {
constexpr wchar_t kClass[]=L"PBZDebugHubWindow_v1",kViewport[]=L"PBZDebugHubViewport_v1",kNav[]=L"PBZDebugHubNav_v1";
constexpr wchar_t kSegment[]=L"PBZDebugHubSegment_v1",kClose[]=L"PBZDebugHubClose_v1",kRail[]=L"PBZDebugHubRail_v1",kToast[]=L"PBZDebugHubToast_v1";
constexpr wchar_t kPause[]=L"PBZDebugHubPause_v1";
constexpr wchar_t kWidth[]=L"PBZHub.MinWidth",kHeight[]=L"PBZHub.MinHeight";
constexpr wchar_t kTitle[]=L"豪之刃";
constexpr int kLanguage=9101,kCloseId=9102;
constexpr size_t kPages=static_cast<size_t>(Page::Count);
// Header subtitle and nav glyph (Segoe Fluent Icons / MDL2) for each page.
constexpr const wchar_t* kSubtitles[kPages]={
    L"传送、直接触战与连战流程，附难度 / 周目设置",
    L"读取并调整当前 Boss 实例的原生 AI 参数",
    L"主角兼容装束的完整装配替换",
    L"以沐小葵 / 魔渊的原 NPC 外观操作主角（实验）",
    L"空中施放、跳跃取消与近敌 JC 奖励",
    L"选择各 Boss 的原生投技，三角 + 圈触发",
    L"头发表示切换、强制 Groom 与调试开关",
    L"触控板滑动切换两套装备，切换前自动记忆",
    L"键盘、Xbox 与 PlayStation 组合键",
};
constexpr wchar_t kGlyphs[kPages]={0xE945,0xE99A,0xE77B,0xE716,0xE805,0xE7AD,0xE771,0xE8AB,0xE144};
struct Entry { const wchar_t* title=L""; Factory factory; HWND window=nullptr; int width=0,height=0; double x=0,y=0; int target_x=0,target_y=0; };
std::array<Entry,kPages> entries;
HWND window_=nullptr,nav_=nullptr,viewport_=nullptr,segment_=nullptr,close_=nullptr,pause_=nullptr,rails_[2]{};
HWND game_window_=nullptr;
bool enabled_=false,appearance_dirty_=false,reduced_=false;
int selected_=0,dpi_=96;
volatile LONG pending_page_=-1,pending_toggle_=0;
ULONGLONG last_motion_=0;
// Window fade/rise, page slide, header title, nav pill, language knob, close hover, pause switch.
UiMotion::Tween opacity_,rise_,page_,title_,pill_,knob_,close_hover_,pause_knob_,pause_hover_;
std::array<UiMotion::Tween,kPages> nav_hover_;
bool hide_pending_=false,restore_game_=false,layout_dirty_=false,held_open_=false,parked_=false;
volatile LONG native_menu_forward_=0;
volatile LONG64 native_menu_deadline_=0;
SHORT native_o_down_=0;
int applied_alpha_=-1;
int nav_hot_=-1,segment_hot_=-1;
bool close_hot_=false,close_down_=false,pause_hot_=false;
RECT rest_{};
bool moving_=false;
struct Rail { bool hot=false,drag=false; int grab=0; UiMotion::Tween width; };
Rail rail_state_[2];
RECT card_{},view_{};
bool need_[2]{};
// Transient notice shown over the bottom of the content card.
struct Toast {
    HWND window=nullptr; std::wstring text; ULONGLONG shown=0; bool active=false,leaving=false;
    UiMotion::Tween fade,slide; UiSurface::Surface surface; int width=0,height=0,margin=0; POINT last{-1,-1}; int alpha=-1;
};
Toast toast_;

int S(int value) { return MulDiv(value,dpi_,96); }
HFONT Font(UiTheme::Font role) { return UiTheme::GetFont(role,static_cast<UINT>(dpi_)); }
// Game title face for dictionary text; falls back per string when a glyph is missing.
HFONT Display(UiTheme::Font role,const std::wstring& text) { return UiTheme::GetFontFor(role,static_cast<UINT>(dpi_),text.c_str(),static_cast<int>(text.size())); }
using UiTheme::Mix;
void Layout();
void ActiveLayout();
void MotionTick();
void Select(int index);
void ScrollTo(double x,double y);

class Paint {
public:
    explicit Paint(HWND window) : window_(window) {
        target_=BeginPaint(window,&paint_); GetClientRect(window,&area_);
        dc_=CreateCompatibleDC(target_);
        bitmap_=dc_?CreateCompatibleBitmap(target_,(std::max)(1L,area_.right),(std::max)(1L,area_.bottom)):nullptr;
        if (bitmap_) old_=SelectObject(dc_,bitmap_);
        SetBkMode(dc(),TRANSPARENT);
    }
    ~Paint() {
        if (bitmap_) { BitBlt(target_,0,0,area_.right,area_.bottom,dc_,0,0,SRCCOPY); SelectObject(dc_,old_); DeleteObject(bitmap_); }
        if (dc_) DeleteDC(dc_);
        EndPaint(window_,&paint_);
    }
    HDC dc() const { return bitmap_?dc_:target_; }
    const RECT& area() const { return area_; }
private:
    HWND window_; PAINTSTRUCT paint_{}; HDC target_=nullptr,dc_=nullptr; HBITMAP bitmap_=nullptr; HGDIOBJ old_=nullptr; RECT area_{};
};
Gdiplus::Color Argb(COLORREF color,BYTE alpha=255) { return {alpha,GetRValue(color),GetGValue(color),GetBValue(color)}; }
void Path(Gdiplus::GraphicsPath& path,Gdiplus::RectF rect,float radius) {
    const float r=(std::min)(radius,(std::min)(rect.Width,rect.Height)/2),d=r*2;
    if (d<.5f) { path.AddRectangle(rect); return; }
    path.AddArc(rect.X,rect.Y,d,d,180,90); path.AddArc(rect.GetRight()-d,rect.Y,d,d,270,90);
    path.AddArc(rect.GetRight()-d,rect.GetBottom()-d,d,d,0,90); path.AddArc(rect.X,rect.GetBottom()-d,d,d,90,90); path.CloseFigure();
}
void Shape(HDC dc,const RECT& rect,float radius,COLORREF fill,BYTE fill_alpha=255,COLORREF edge=CLR_INVALID,BYTE edge_alpha=255) {
    if (rect.right<=rect.left||rect.bottom<=rect.top) return;
    Gdiplus::Graphics graphics(dc); graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    Gdiplus::GraphicsPath path;
    Path(path,{static_cast<float>(rect.left)+.5f,static_cast<float>(rect.top)+.5f,static_cast<float>(rect.right-rect.left)-1,static_cast<float>(rect.bottom-rect.top)-1},radius);
    if (fill_alpha) { Gdiplus::SolidBrush brush(Argb(fill,fill_alpha)); graphics.FillPath(&brush,&path); }
    if (edge!=CLR_INVALID&&edge_alpha) { Gdiplus::Pen pen(Argb(edge,edge_alpha),1.0f); graphics.DrawPath(&pen,&path); }
}
void Text(HDC dc,const std::wstring& text,RECT rect,HFONT font,COLORREF color,UINT flags) {
    const HGDIOBJ old=SelectObject(dc,font); SetTextColor(dc,color);
    DrawTextW(dc,text.c_str(),static_cast<int>(text.size()),&rect,flags|DT_NOPREFIX); SelectObject(dc,old);
}
void Fill(HDC dc,const RECT& rect,COLORREF color) { SetDCBrushColor(dc,color); FillRect(dc,&rect,static_cast<HBRUSH>(GetStockObject(DC_BRUSH))); }
bool KeyboardCues(HWND window) { return !(SendMessageW(window,WM_QUERYUISTATE,0,0)&UISF_HIDEFOCUS); }

// ---- geometry -------------------------------------------------------------
int SidebarWidth() { return S(224); }
int NavTop() { return S(86); }
int NavItem() { return S(42); }
int NavStep() { return S(46); }
RECT NavItemRect(int index) {
    RECT client{}; if (nav_) GetClientRect(nav_,&client);
    return {0,index*NavStep(),client.right,index*NavStep()+NavItem()};
}
RECT CardRect() {
    RECT client{}; GetClientRect(window_,&client);
    return {SidebarWidth(),S(78),client.right-S(14),client.bottom-S(14)};
}

// ---- language / text -------------------------------------------------------
std::wstring Tr(const wchar_t* source) { return UiI18n::Translate(source); }
void RelayoutPages() {
    for (Entry& entry:entries) if (entry.window) {
        RECT client{}; GetClientRect(entry.window,&client);
        SendMessageW(entry.window,WM_SIZE,SIZE_RESTORED,MAKELPARAM(client.right,client.bottom));
    }
}
void RefreshLanguage() {
    if (!window_) return;
    UiI18n::SetText(window_,kTitle);
    knob_.Set(UiI18n::GetLanguage()==UiI18n::Language::English?1:0,220,GetTickCount64(),reduced_);
    RelayoutPages(); ActiveLayout();
    InvalidateRect(window_,nullptr,FALSE); InvalidateRect(nav_,nullptr,FALSE); InvalidateRect(segment_,nullptr,FALSE);
}
void SetLanguage(int index) {
    const auto wanted=index==1?UiI18n::Language::English:UiI18n::Language::SimplifiedChinese;
    if (wanted!=UiI18n::GetLanguage()) UiI18n::SetLanguage(wanted);
    RefreshLanguage();
}

// ---- game window ------------------------------------------------------------
bool GameWindow(HWND window) {
    DWORD process=0; wchar_t type[96]{};
    return window&&IsWindow(window)&&GetWindowThreadProcessId(window,&process)&&process==GetCurrentProcessId()&&
        GetClassNameW(window,type,96)&&!_wcsicmp(type,L"UnrealWindow");
}
BOOL CALLBACK FindGameWindow(HWND window,LPARAM result) {
    if (!GameWindow(window)||!IsWindowVisible(window)) return TRUE;
    *reinterpret_cast<HWND*>(result)=window; return FALSE;
}
HWND CurrentGameWindow() {
    const HWND game=UiHost::Game();
    if (game) game_window_=game;
    else if (!GameWindow(game_window_)) { game_window_=nullptr; EnumWindows(FindGameWindow,reinterpret_cast<LPARAM>(&game_window_)); }
    return game_window_;
}
bool ChildWindow() { return window_&&(GetWindowLongPtrW(window_,GWL_STYLE)&WS_CHILD)!=0; }
RECT FromScreen(const RECT& screen) {
    if (!ChildWindow()) return screen;
    const POINT origin=UiHost::ClientOrigin();
    return {screen.left-origin.x,screen.top-origin.y,screen.right-origin.x,screen.bottom-origin.y};
}
void FocusHub() {
    if (!nav_) return;
    const HWND game=UiHost::Game();
    const DWORD other=game?GetWindowThreadProcessId(game,nullptr):0;
    const DWORD self=GetCurrentThreadId();
    const BOOL linked=other&&other!=self?AttachThreadInput(self,other,TRUE):FALSE;
    SetFocus(nav_);
    if (linked) AttachThreadInput(self,other,FALSE);
}
// The hub is a child of UnrealWindow on another thread, so both threads share
// one focus. A hidden hub must never keep it: the game would stop getting keys.
bool FocusInHub(HWND focus) {
    if (!focus||!window_) return false;
    if (focus==window_||IsChild(window_,focus)) return true;
    return toast_.window&&focus==toast_.window;
}
void ReturnFocus() {
    const HWND game=UiHost::Game();
    if (!game||!FocusInHub(GetFocus())) return;
    const DWORD other=GetWindowThreadProcessId(game,nullptr),self=GetCurrentThreadId();
    const BOOL linked=other&&other!=self?AttachThreadInput(self,other,TRUE):FALSE;
    SetFocus(game);
    if (linked) AttachThreadInput(self,other,FALSE);
}
bool HubFocused() {
    if (!window_) return false;
    if (UiHost::FocusInside(window_)) return true;
    const HWND foreground=GetForegroundWindow();
    return foreground==window_||(foreground&&IsChild(window_,foreground));
}
void KeepInside() {
    if (!window_||!held_open_||hide_pending_) return;
    if (!UiHost::InGame()) {
        if (!parked_) {
            ShowWindow(window_,SW_HIDE); UiGlass::Hide(window_);
            if (toast_.window) ShowWindow(toast_.window,SW_HIDE);
            parked_=true;
        }
        return;
    }
    if (GetParent(window_)!=UiHost::Game()) UiHost::Attach(window_);
    if (parked_) { parked_=false; ShowWindow(window_,SW_SHOWNA); UiGlass::Sync(window_); }
    const ULONGLONG now=GetTickCount64();
    if (opacity_.Running(now)||rise_.Running(now)) return;
    const SIZE client=UiHost::ClientSize();
    if (client.cx<=0||client.cy<=0) return;
    RECT next=rest_;
    int width=next.right-next.left,height=next.bottom-next.top;
    if (width<1) width=1;
    if (height<1) height=1;
    if (width>client.cx) width=client.cx;
    if (height>client.cy) height=client.cy;
    if (next.left<0) next.left=0;
    if (next.top<0) next.top=0;
    if (next.left+width>client.cx) next.left=client.cx-width;
    if (next.top+height>client.cy) next.top=client.cy-height;
    next.right=next.left+width; next.bottom=next.top+height;
    if (EqualRect(&next,&rest_)) return;
    rest_=next; moving_=true;
    SetWindowPos(window_,nullptr,rest_.left,rest_.top,width,height,SWP_NOZORDER|SWP_NOACTIVATE);
    moving_=false; UiGlass::Sync(window_);
}

// ---- page viewport and scroll rails ----------------------------------------
POINT ContentSize(const Entry& page) {
    return {(std::max)(page.width,static_cast<int>(view_.right-view_.left)),(std::max)(page.height,static_cast<int>(view_.bottom-view_.top))};
}
void PositionPage() {
    Entry& page=entries[static_cast<size_t>(selected_)]; if (!page.window) return;
    const int offset=static_cast<int>(std::lround(page_.At(GetTickCount64())));
    SetWindowPos(page.window,nullptr,-static_cast<int>(std::lround(page.x)),-static_cast<int>(std::lround(page.y))+offset,0,0,
        SWP_NOSIZE|SWP_NOZORDER|SWP_NOACTIVATE);
}
void ActiveLayout() {
    if (!viewport_||!window_) return;
    Entry& page=entries[static_cast<size_t>(selected_)];
    card_=CardRect();
    RECT inner=card_; InflateRect(&inner,-S(6),-S(6));
    const int rail=S(12);
    const int full_w=inner.right-inner.left,full_h=inner.bottom-inner.top;
    bool horizontal=false,vertical=false;
    for (int i=0;i<2;++i) {
        horizontal=page.width>full_w-(vertical?rail:0);
        vertical=page.height>full_h-(horizontal?rail:0);
    }
    need_[0]=vertical; need_[1]=horizontal;
    view_={inner.left,inner.top,inner.right-(vertical?rail:0),inner.bottom-(horizontal?rail:0)};
    SetWindowPos(viewport_,nullptr,view_.left,view_.top,(std::max)(1L,view_.right-view_.left),(std::max)(1L,view_.bottom-view_.top),SWP_NOZORDER|SWP_NOACTIVATE);
    if (rails_[0]) SetWindowPos(rails_[0],nullptr,view_.right+S(2),view_.top,rail-S(2),view_.bottom-view_.top,SWP_NOZORDER|SWP_NOACTIVATE|(vertical?SWP_SHOWWINDOW:SWP_HIDEWINDOW));
    if (rails_[1]) SetWindowPos(rails_[1],nullptr,view_.left,view_.bottom+S(2),view_.right-view_.left,rail-S(2),SWP_NOZORDER|SWP_NOACTIVATE|(horizontal?SWP_SHOWWINDOW:SWP_HIDEWINDOW));
    const POINT content=ContentSize(page);
    const int max_x=(std::max)(0,static_cast<int>(content.x-(view_.right-view_.left))),max_y=(std::max)(0,static_cast<int>(content.y-(view_.bottom-view_.top)));
    page.target_x=std::clamp(page.target_x,0,max_x); page.target_y=std::clamp(page.target_y,0,max_y);
    page.x=std::clamp(page.x,0.0,static_cast<double>(max_x)); page.y=std::clamp(page.y,0.0,static_cast<double>(max_y));
    if (page.window) {
        RECT current{}; GetClientRect(page.window,&current);
        if (current.right!=content.x||current.bottom!=content.y)
            SetWindowPos(page.window,nullptr,0,0,content.x,content.y,SWP_NOMOVE|SWP_NOZORDER|SWP_NOACTIVATE);
        PositionPage();
    }
    for (HWND rail_window:rails_) if (rail_window) InvalidateRect(rail_window,nullptr,FALSE);
}
void ScrollTo(double x,double y) {
    Entry& page=entries[static_cast<size_t>(selected_)];
    const POINT content=ContentSize(page);
    page.target_x=std::clamp(static_cast<int>(std::lround(x)),0,(std::max)(0,static_cast<int>(content.x-(view_.right-view_.left))));
    page.target_y=std::clamp(static_cast<int>(std::lround(y)),0,(std::max)(0,static_cast<int>(content.y-(view_.bottom-view_.top))));
    if (reduced_) { page.x=page.target_x; page.y=page.target_y; PositionPage(); }
    for (HWND rail_window:rails_) if (rail_window) InvalidateRect(rail_window,nullptr,FALSE);
}
void Wheel(int delta,bool horizontal) {
    const Entry& page=entries[static_cast<size_t>(selected_)];
    const double step=static_cast<double>(delta)/WHEEL_DELTA*S(84);
    if (horizontal||!need_[0]) ScrollTo(page.target_x-step,page.target_y);
    else ScrollTo(page.target_x,page.target_y-step);
}
void RevealFocus() {
    if (!viewport_||page_.Running(GetTickCount64())) return;
    Entry& page=entries[static_cast<size_t>(selected_)]; const HWND focus=GetFocus();
    if (!page.window||!focus||!IsChild(page.window,focus)) return;
    static HWND last=nullptr; if (focus==last) return; last=focus;
    RECT rect{},area{}; GetWindowRect(focus,&rect); GetClientRect(viewport_,&area);
    MapWindowPoints(nullptr,viewport_,reinterpret_cast<POINT*>(&rect),2);
    const int width=(std::min)(static_cast<int>(rect.right-rect.left),(std::max)(1,static_cast<int>(area.right)-S(16)));
    const int height=(std::min)(static_cast<int>(rect.bottom-rect.top),(std::max)(1,static_cast<int>(area.bottom)-S(16)));
    double x=page.target_x,y=page.target_y;
    if (rect.left<0) x+=rect.left-S(8); else if (rect.left+width>area.right) x+=rect.left+width-area.right+S(8);
    if (rect.top<0) y+=rect.top-S(8); else if (rect.top+height>area.bottom) y+=rect.top+height-area.bottom+S(8);
    if (x!=page.target_x||y!=page.target_y) ScrollTo(x,y);
}
struct Thumb { int start=0,length=0,track=0,range=0; };
Thumb ThumbOf(int axis) {
    Thumb thumb; if (!rails_[axis]) return thumb;
    const Entry& page=entries[static_cast<size_t>(selected_)]; const POINT content=ContentSize(page);
    RECT client{}; GetClientRect(rails_[axis],&client);
    thumb.track=axis==0?client.bottom:client.right;
    const int view=axis==0?view_.bottom-view_.top:view_.right-view_.left,total=axis==0?content.y:content.x;
    thumb.range=(std::max)(0,total-view);
    thumb.length=total>0?(std::max)(S(40),static_cast<int>(static_cast<long long>(thumb.track)*view/total)):thumb.track;
    thumb.length=(std::min)(thumb.length,thumb.track);
    const double position=axis==0?page.y:page.x;
    thumb.start=thumb.range>0?static_cast<int>(std::lround((thumb.track-thumb.length)*position/thumb.range)):0;
    return thumb;
}
LRESULT CALLBACK RailProc(HWND hwnd,UINT message,WPARAM wparam,LPARAM lparam) {
    const int axis=hwnd==rails_[1]?1:0; Rail& rail=rail_state_[axis];
    switch (message) {
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: {
        Paint paint(hwnd); HDC dc=paint.dc(); RECT client=paint.area();
        Fill(dc,client,UiTheme::Surface());
        const Thumb thumb=ThumbOf(axis);
        const double grow=rail.width.At(GetTickCount64());
        const int thickness=static_cast<int>(std::lround(S(4)+(S(8)-S(4))*grow));
        const int across=axis==0?client.right:client.bottom,offset=(across-thickness)/2;
        if (grow>.01) {
            RECT track=axis==0?RECT{offset,0,offset+thickness,client.bottom}:RECT{0,offset,client.right,offset+thickness};
            Shape(dc,track,thickness/2.0f,UiTheme::Panel(),static_cast<BYTE>(200*grow));
        }
        RECT shape=axis==0?RECT{offset,thumb.start,offset+thickness,thumb.start+thumb.length}:RECT{thumb.start,offset,thumb.start+thumb.length,offset+thickness};
        const COLORREF color=Mix(Mix(UiTheme::Border(),UiTheme::MutedText(),static_cast<int>(35+45*grow)),UiTheme::Accent(),rail.drag?45:0);
        Shape(dc,shape,thickness/2.0f,color);
        return 0;
    }
    case WM_MOUSEMOVE: {
        if (!rail.hot) {
            rail.hot=true; rail.width.Set(1,160,GetTickCount64(),reduced_);
            TRACKMOUSEEVENT track{sizeof(track),TME_LEAVE,hwnd,0}; TrackMouseEvent(&track);
        }
        if (rail.drag) {
            const Thumb thumb=ThumbOf(axis); const int at=axis==0?GET_Y_LPARAM(lparam):GET_X_LPARAM(lparam);
            const int free=(std::max)(1,thumb.track-thumb.length);
            const double value=static_cast<double>(at-rail.grab)*thumb.range/free;
            Entry& page=entries[static_cast<size_t>(selected_)];
            if (axis==0) { page.target_y=std::clamp(static_cast<int>(std::lround(value)),0,thumb.range); page.y=page.target_y; }
            else { page.target_x=std::clamp(static_cast<int>(std::lround(value)),0,thumb.range); page.x=page.target_x; }
            PositionPage(); InvalidateRect(hwnd,nullptr,FALSE);
        }
        return 0;
    }
    case WM_MOUSELEAVE:
        rail.hot=false; if (!rail.drag) rail.width.Set(0,260,GetTickCount64(),reduced_); return 0;
    case WM_LBUTTONDOWN: {
        const Thumb thumb=ThumbOf(axis); const int at=axis==0?GET_Y_LPARAM(lparam):GET_X_LPARAM(lparam);
        const Entry& page=entries[static_cast<size_t>(selected_)];
        if (at>=thumb.start&&at<thumb.start+thumb.length) { rail.drag=true; rail.grab=at-thumb.start; SetCapture(hwnd); }
        else {
            const int view=axis==0?view_.bottom-view_.top:view_.right-view_.left,step=static_cast<int>(view*.9)*(at<thumb.start?-1:1);
            if (axis==0) ScrollTo(page.target_x,page.target_y+step); else ScrollTo(page.target_x+step,page.target_y);
        }
        InvalidateRect(hwnd,nullptr,FALSE); return 0;
    }
    case WM_LBUTTONUP: case WM_CAPTURECHANGED:
        if (rail.drag) { rail.drag=false; if (GetCapture()==hwnd) ReleaseCapture(); if (!rail.hot) rail.width.Set(0,260,GetTickCount64(),reduced_); InvalidateRect(hwnd,nullptr,FALSE); }
        return 0;
    case WM_MOUSEWHEEL: Wheel(GET_WHEEL_DELTA_WPARAM(wparam),axis==1); return 0;
    case WM_MOUSEHWHEEL: Wheel(-GET_WHEEL_DELTA_WPARAM(wparam),true); return 0;
    default: return DefWindowProcW(hwnd,message,wparam,lparam);
    }
}
LRESULT CALLBACK ViewportProc(HWND hwnd,UINT message,WPARAM wparam,LPARAM lparam) {
    if (message==WM_MOUSEWHEEL) { Wheel(GET_WHEEL_DELTA_WPARAM(wparam),(GetKeyState(VK_SHIFT)&0x8000)!=0); return 0; }
    if (message==WM_MOUSEHWHEEL) { Wheel(-GET_WHEEL_DELTA_WPARAM(wparam),true); return 0; }
    if (message==WM_ERASEBKGND) { RECT client{}; GetClientRect(hwnd,&client); FillRect(reinterpret_cast<HDC>(wparam),&client,UiTheme::Background()); return 1; }
    return DefWindowProcW(hwnd,message,wparam,lparam);
}

// ---- sidebar navigation -------------------------------------------------------
int NavHit(LPARAM lparam) {
    const POINT point{GET_X_LPARAM(lparam),GET_Y_LPARAM(lparam)};
    for (int i=0;i<static_cast<int>(kPages);++i) { const RECT rect=NavItemRect(i); if (PtInRect(&rect,point)) return i; }
    return -1;
}
void NavHover(int index) {
    if (index==nav_hot_) return;
    const ULONGLONG now=GetTickCount64();
    if (nav_hot_>=0) nav_hover_[static_cast<size_t>(nav_hot_)].Set(0,220,now,reduced_);
    nav_hot_=index;
    if (index>=0) nav_hover_[static_cast<size_t>(index)].Set(1,120,now,reduced_);
    InvalidateRect(nav_,nullptr,FALSE);
}
void PaintNav(HWND hwnd) {
    Paint paint(hwnd); HDC dc=paint.dc(); const RECT client=paint.area();
    const ULONGLONG now=GetTickCount64();
    Fill(dc,client,UiTheme::Base());
    const double pill=pill_.At(now);
    const bool art=!UiTheme::HighContrast();
    auto brush=[&](const RECT& rect,float opacity) {
        Gdiplus::Graphics graphics(dc);
        UiAssets::DrawBar(graphics,UiAssets::Id::SelectedBar,Gdiplus::RectF(static_cast<float>(rect.left),static_cast<float>(rect.top+S(3)),
            static_cast<float>(rect.right-rect.left),static_cast<float>(rect.bottom-rect.top-S(6))),opacity);
    };
    for (int i=0;i<static_cast<int>(kPages);++i) {
        const double hover=nav_hover_[static_cast<size_t>(i)].At(now);
        if (hover>.01&&i!=selected_) { if (art) brush(NavItemRect(i),static_cast<float>(.14*hover)); else Shape(dc,NavItemRect(i),static_cast<float>(S(9)),UiTheme::Panel(),static_cast<BYTE>(150*hover)); }
    }
    RECT active=NavItemRect(0); OffsetRect(&active,0,static_cast<int>(std::lround(pill*NavStep())));
    // The game's selected-row brush stroke rides along with the slide.
    if (art) brush(active,1.f);
    else Shape(dc,active,static_cast<float>(S(9)),Mix(UiTheme::Panel(),UiTheme::Selection(),55),255,Mix(UiTheme::Border(),UiTheme::Base(),35));
    if (!art&&GetFocus()==hwnd&&KeyboardCues(hwnd)) {
        RECT focus=NavItemRect(selected_); InflateRect(&focus,-S(1),-S(1));
        Shape(dc,focus,static_cast<float>(S(8)),0,0,UiTheme::Accent(),150);
    }
    for (int i=0;i<static_cast<int>(kPages);++i) {
        const RECT rect=NavItemRect(i);
        const double hover=nav_hover_[static_cast<size_t>(i)].At(now);
        const double closeness=(std::max)(0.0,1.0-std::fabs(pill-i));
        const double emphasis=(std::max)(closeness,hover*.6);
        const COLORREF ink=RGB(0x14,0x12,0x10);
        COLORREF label=Mix(UiTheme::MutedText(),UiTheme::Text(),static_cast<int>(100*emphasis));
        COLORREF icon=Mix(Mix(UiTheme::MutedText(),UiTheme::Text(),static_cast<int>(60*hover)),UiTheme::Accent(),static_cast<int>(100*closeness));
        if (art) { label=Mix(label,ink,static_cast<int>(100*closeness)); icon=Mix(icon,ink,static_cast<int>(100*closeness)); }
        RECT glyph{rect.left+S(14),rect.top,rect.left+S(14)+S(22),rect.bottom};
        Text(dc,std::wstring(1,kGlyphs[i]),glyph,Font(UiTheme::Font::Icon),icon,DT_SINGLELINE|DT_VCENTER|DT_CENTER);
        RECT caption{rect.left+S(48),rect.top,rect.right-S(10),rect.bottom};
        const std::wstring title=Tr(entries[static_cast<size_t>(i)].title);
        Text(dc,title,caption,Display(UiTheme::Font::Heading,title),label,DT_SINGLELINE|DT_VCENTER|DT_LEFT|DT_END_ELLIPSIS);
    }
}
LRESULT CALLBACK NavProc(HWND hwnd,UINT message,WPARAM wparam,LPARAM lparam) {
    switch (message) {
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: PaintNav(hwnd); return 0;
    case WM_GETDLGCODE: return DLGC_WANTARROWS;
    case WM_SETFOCUS: case WM_KILLFOCUS: case WM_UPDATEUISTATE: {
        const LRESULT result=DefWindowProcW(hwnd,message,wparam,lparam); InvalidateRect(hwnd,nullptr,FALSE); return result;
    }
    case WM_MOUSEMOVE: {
        NavHover(NavHit(lparam));
        TRACKMOUSEEVENT track{sizeof(track),TME_LEAVE,hwnd,0}; TrackMouseEvent(&track); return 0;
    }
    case WM_MOUSELEAVE: NavHover(-1); return 0;
    case WM_SETCURSOR:
        if (LOWORD(lparam)==HTCLIENT) { POINT point{}; GetCursorPos(&point); ScreenToClient(hwnd,&point);
            if (NavHit(MAKELPARAM(point.x,point.y))>=0) { SetCursor(LoadCursorW(nullptr,IDC_HAND)); return TRUE; } }
        break;
    case WM_LBUTTONDOWN: { const int hit=NavHit(lparam); SetFocus(hwnd); if (hit>=0) Select(hit); return 0; }
    case WM_KEYDOWN: {
        int next=selected_;
        if (wparam==VK_UP) --next; else if (wparam==VK_DOWN) ++next;
        else if (wparam==VK_HOME) next=0; else if (wparam==VK_END) next=static_cast<int>(kPages)-1;
        else break;
        next=(next+static_cast<int>(kPages))%static_cast<int>(kPages);
        SendMessageW(hwnd,WM_CHANGEUISTATE,MAKEWPARAM(UIS_CLEAR,UISF_HIDEFOCUS),0);
        Select(next); SetFocus(hwnd); return 0;
    }
    default: break;
    }
    return DefWindowProcW(hwnd,message,wparam,lparam);
}

// ---- language segment and close button ---------------------------------------
int SegmentHit(HWND hwnd,LPARAM lparam) { RECT client{}; GetClientRect(hwnd,&client); return GET_X_LPARAM(lparam)<client.right/2?0:1; }
LRESULT CALLBACK SegmentProc(HWND hwnd,UINT message,WPARAM wparam,LPARAM lparam) {
    const int current=UiI18n::GetLanguage()==UiI18n::Language::English?1:0;
    switch (message) {
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: {
        Paint paint(hwnd); HDC dc=paint.dc(); const RECT client=paint.area();
        Fill(dc,client,UiTheme::Base());
        Shape(dc,client,client.bottom/2.0f,Mix(UiTheme::Base(),UiTheme::Panel(),70),255,Mix(UiTheme::Border(),UiTheme::Base(),25));
        const double knob=knob_.At(GetTickCount64()); const int half=client.right/2,pad=S(3);
        RECT thumb{pad+static_cast<int>(std::lround(knob*(half-pad))),pad,0,client.bottom-pad};
        thumb.right=thumb.left+half-pad;
        Shape(dc,thumb,(thumb.bottom-thumb.top)/2.0f,Mix(UiTheme::Panel(),UiTheme::Selection(),70),255,Mix(UiTheme::Accent(),UiTheme::Panel(),55));
        if (GetFocus()==hwnd&&KeyboardCues(hwnd)) Shape(dc,client,client.bottom/2.0f,0,0,UiTheme::Accent(),170);
        const wchar_t* labels[2]={L"中文",L"EN"};
        for (int i=0;i<2;++i) {
            const double on=i==0?1.0-knob:knob; const bool hot=segment_hot_==i;
            const COLORREF color=Mix(hot?Mix(UiTheme::MutedText(),UiTheme::Text(),55):UiTheme::MutedText(),UiTheme::Text(),static_cast<int>(100*on));
            Text(dc,labels[i],{i*half,0,(i+1)*half,client.bottom},Font(on>.5?UiTheme::Font::Strong:UiTheme::Font::Small),color,DT_SINGLELINE|DT_VCENTER|DT_CENTER);
        }
        return 0;
    }
    case WM_GETDLGCODE: return DLGC_WANTARROWS;
    case CB_GETCURSEL: return current;
    case CB_SETCURSEL: if (wparam<=1) { knob_.Set(static_cast<double>(wparam),220,GetTickCount64(),reduced_); InvalidateRect(hwnd,nullptr,FALSE); } return static_cast<LRESULT>(wparam);
    case WM_SETFOCUS: case WM_KILLFOCUS: InvalidateRect(hwnd,nullptr,FALSE); break;
    case WM_MOUSEMOVE: {
        const int hit=SegmentHit(hwnd,lparam); if (hit!=segment_hot_) { segment_hot_=hit; InvalidateRect(hwnd,nullptr,FALSE); }
        TRACKMOUSEEVENT track{sizeof(track),TME_LEAVE,hwnd,0}; TrackMouseEvent(&track); return 0;
    }
    case WM_MOUSELEAVE: segment_hot_=-1; InvalidateRect(hwnd,nullptr,FALSE); return 0;
    case WM_SETCURSOR: if (LOWORD(lparam)==HTCLIENT) { SetCursor(LoadCursorW(nullptr,IDC_HAND)); return TRUE; } break;
    case WM_LBUTTONDOWN: SetLanguage(SegmentHit(hwnd,lparam)); return 0;
    case WM_KEYDOWN:
        if (wparam==VK_LEFT||wparam==VK_RIGHT||wparam==VK_SPACE||wparam==VK_RETURN) {
            SetLanguage(wparam==VK_LEFT?0:wparam==VK_RIGHT?1:1-current); return 0;
        }
        break;
    default: break;
    }
    return DefWindowProcW(hwnd,message,wparam,lparam);
}
// "Pause game" switch: label plus a small track; the knob slides on toggle.
void TogglePause() {
    HubPause::SetEnabled(!HubPause::Enabled());
    pause_knob_.ease=UiMotion::EaseOutCubic; pause_knob_.Set(HubPause::Enabled()?1:0,220,GetTickCount64(),reduced_);
    if (pause_) InvalidateRect(pause_,nullptr,FALSE);
}
LRESULT CALLBACK PauseProc(HWND hwnd,UINT message,WPARAM wparam,LPARAM lparam) {
    switch (message) {
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: {
        Paint paint(hwnd); HDC dc=paint.dc(); const RECT client=paint.area();
        Fill(dc,client,UiTheme::Base());
        const ULONGLONG now=GetTickCount64();
        const double hover=pause_hover_.At(now),on=pause_knob_.At(now);
        Shape(dc,client,client.bottom/2.0f,Mix(UiTheme::Base(),UiTheme::Panel(),static_cast<int>(std::lround(70+20*hover))),255,
            Mix(Mix(UiTheme::Border(),UiTheme::Base(),25),UiTheme::Accent(),static_cast<int>(std::lround(30*on))));
        if (GetFocus()==hwnd&&KeyboardCues(hwnd)) Shape(dc,client,client.bottom/2.0f,0,0,UiTheme::Accent(),170);
        const int track_w=S(30),track_h=S(16),pad=S(9);
        RECT track{client.right-pad-track_w,(client.bottom-track_h)/2,client.right-pad,(client.bottom+track_h)/2};
        Shape(dc,track,track_h/2.0f,Mix(Mix(UiTheme::Base(),UiTheme::Border(),60),UiTheme::Accent(),static_cast<int>(std::lround(85*on))),255);
        const int knob=track_h-S(4),travel=track_w-knob-S(4);
        RECT dot{track.left+S(2)+static_cast<int>(std::lround(on*travel)),track.top+S(2),0,track.bottom-S(2)}; dot.right=dot.left+knob;
        Shape(dc,dot,knob/2.0f,Mix(UiTheme::MutedText(),RGB(0x14,0x12,0x10),static_cast<int>(std::lround(100*on))),255);
        const COLORREF label=Mix(UiTheme::MutedText(),UiTheme::Text(),static_cast<int>(std::lround(35+40*on+25*hover)));
        Text(dc,Tr(L"暂停游戏"),{S(12),0,track.left-S(6),client.bottom},Font(UiTheme::Font::Small),label,DT_SINGLELINE|DT_VCENTER|DT_LEFT|DT_END_ELLIPSIS);
        return 0;
    }
    case WM_GETDLGCODE: return DLGC_WANTCHARS;
    case WM_SETFOCUS: case WM_KILLFOCUS: InvalidateRect(hwnd,nullptr,FALSE); break;
    case WM_MOUSEMOVE:
        if (!pause_hot_) { pause_hot_=true; pause_hover_.Set(1,120,GetTickCount64(),reduced_); TRACKMOUSEEVENT track{sizeof(track),TME_LEAVE,hwnd,0}; TrackMouseEvent(&track); }
        return 0;
    case WM_MOUSELEAVE: pause_hot_=false; pause_hover_.Set(0,200,GetTickCount64(),reduced_); return 0;
    case WM_SETCURSOR: if (LOWORD(lparam)==HTCLIENT) { SetCursor(LoadCursorW(nullptr,IDC_HAND)); return TRUE; } break;
    case WM_LBUTTONDOWN: TogglePause(); return 0;
    case WM_KEYDOWN: if (wparam==VK_SPACE||wparam==VK_RETURN) { TogglePause(); return 0; } break;
    default: break;
    }
    return DefWindowProcW(hwnd,message,wparam,lparam);
}
LRESULT CALLBACK CloseProc(HWND hwnd,UINT message,WPARAM wparam,LPARAM lparam) {
    switch (message) {
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: {
        Paint paint(hwnd); HDC dc=paint.dc(); const RECT client=paint.area();
        Fill(dc,client,UiTheme::Base());
        const double hover=close_hover_.At(GetTickCount64());
        if (hover>.01) Shape(dc,client,static_cast<float>(S(8)),Mix(UiTheme::Danger(),UiTheme::Base(),close_down_?25:0),static_cast<BYTE>(235*hover));
        Gdiplus::Graphics graphics(dc); graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        Gdiplus::Pen pen(Argb(Mix(UiTheme::MutedText(),RGB(255,255,255),static_cast<int>(100*hover))),(std::max)(1.2f,S(10)/7.0f));
        pen.SetStartCap(Gdiplus::LineCapRound); pen.SetEndCap(Gdiplus::LineCapRound);
        const float cx=client.right/2.0f,cy=client.bottom/2.0f,r=S(5)+.5f;
        graphics.DrawLine(&pen,cx-r,cy-r,cx+r,cy+r); graphics.DrawLine(&pen,cx-r,cy+r,cx+r,cy-r);
        return 0;
    }
    case WM_MOUSEMOVE:
        if (!close_hot_) { close_hot_=true; close_hover_.Set(1,120,GetTickCount64(),reduced_); TRACKMOUSEEVENT track{sizeof(track),TME_LEAVE,hwnd,0}; TrackMouseEvent(&track); }
        return 0;
    case WM_MOUSELEAVE: close_hot_=false; close_down_=false; close_hover_.Set(0,200,GetTickCount64(),reduced_); return 0;
    case WM_LBUTTONDOWN: close_down_=true; SetCapture(hwnd); InvalidateRect(hwnd,nullptr,FALSE); return 0;
    case WM_LBUTTONUP: {
        const bool click=close_down_&&close_hot_; close_down_=false; if (GetCapture()==hwnd) ReleaseCapture();
        InvalidateRect(hwnd,nullptr,FALSE); if (click) Hide(); return 0;
    }
    default: return DefWindowProcW(hwnd,message,wparam,lparam);
    }
}

// ---- toast -------------------------------------------------------------------------
constexpr DWORD kToastLife=4200;
void RenderToast() {
    Toast& t=toast_; t.margin=S(18);
    const HFONT font=Font(UiTheme::Font::Body);
    HDC screen=GetDC(nullptr); HDC measure=CreateCompatibleDC(screen); ReleaseDC(nullptr,screen);
    const HGDIOBJ old=SelectObject(measure,font);
    const RECT card=CardRect();
    const int limit=(std::max)(S(240),(std::min)(S(560),static_cast<int>(card.right-card.left)-S(120)));
    RECT text{0,0,limit,0}; DrawTextW(measure,t.text.c_str(),-1,&text,DT_CALCRECT|DT_WORDBREAK|DT_NOPREFIX);
    SelectObject(measure,old); DeleteDC(measure);
    t.width=text.right+S(16+22+12+20); t.height=(std::max)(S(50),static_cast<int>(text.bottom)+S(28));
    if (!t.surface.Ensure(t.width+2*t.margin,t.height+2*t.margin)) return;
    t.surface.Clear(); HDC dc=t.surface.dc; SetBkMode(dc,TRANSPARENT);
    const RECT body{t.margin,t.margin,t.margin+t.width,t.margin+t.height};
    Fill(dc,body,Mix(UiTheme::Panel(),UiTheme::Text(),5));
    RECT glyph{body.left+S(16),body.top,body.left+S(16)+S(22),body.bottom};
    Text(dc,std::wstring(1,static_cast<wchar_t>(0xE946)),glyph,Font(UiTheme::Font::Icon),UiTheme::Accent(),DT_SINGLELINE|DT_VCENTER|DT_CENTER);
    RECT label{glyph.right+S(12),body.top+(t.height-text.bottom)/2,body.right-S(20),body.bottom};
    Text(dc,t.text,label,font,UiTheme::Text(),DT_WORDBREAK|DT_LEFT);
    t.surface.Shape(body,S(12),Mix(UiTheme::Border(),UiTheme::Accent(),35),t.margin,.5);
    t.last={-1,-1}; t.alpha=-1;
}
void ToastTick(ULONGLONG now) {
    Toast& t=toast_; if (!t.active||!t.window) return;
    if (!window_||!IsWindowVisible(window_)||hide_pending_) { ShowWindow(t.window,SW_HIDE); t.active=false; return; }
    if (!t.leaving&&now>=t.shown+kToastLife) {
        t.leaving=true; t.fade.ease=UiMotion::EaseInCubic; t.fade.Set(0,220,now,reduced_); t.slide.Set(S(8),220,now,reduced_);
    }
    if (t.leaving&&!t.fade.Running(now)) { ShowWindow(t.window,SW_HIDE); t.active=false; return; }
    RECT card=CardRect(); MapWindowPoints(window_,nullptr,reinterpret_cast<POINT*>(&card),2);
    const POINT position{(card.left+card.right)/2-t.width/2-t.margin,card.bottom-S(24)-t.height-t.margin+static_cast<int>(std::lround(t.slide.At(now)))};
    const int alpha=static_cast<int>(std::clamp(std::lround(t.fade.At(now)*255),0L,255L));
    if (position.x!=t.last.x||position.y!=t.last.y||alpha!=t.alpha) { t.surface.Present(t.window,position,static_cast<BYTE>(alpha)); t.last=position; t.alpha=alpha; }
}
LRESULT CALLBACK ToastProc(HWND hwnd,UINT message,WPARAM wparam,LPARAM lparam) {
    if (message==WM_MOUSEACTIVATE) return MA_NOACTIVATE;
    if (message==WM_SETCURSOR) { SetCursor(LoadCursorW(nullptr,IDC_HAND)); return TRUE; }
    if (message==WM_LBUTTONUP&&toast_.active&&!toast_.leaving) { toast_.shown=GetTickCount64()-kToastLife; return 0; }
    return DefWindowProcW(hwnd,message,wparam,lparam);
}

// ---- hub window ----------------------------------------------------------------
void PaintHub(HWND hwnd) {
    Paint paint(hwnd); HDC dc=paint.dc(); const RECT client=paint.area();
    const ULONGLONG now=GetTickCount64();
    Fill(dc,client,UiTheme::Base());
    // Brand mark: the avatar in a circle with a pale-gold ring, then the name.
    const RECT tile{S(20),S(20),S(20)+S(44),S(20)+S(44)};
    {
        Gdiplus::Graphics graphics(dc);
        UiAssets::DrawAvatar(graphics,Gdiplus::RectF(static_cast<float>(tile.left),static_cast<float>(tile.top),static_cast<float>(S(44)),static_cast<float>(S(44))),
            UiTheme::Accent(),static_cast<float>((std::max)(1,S(2))));
    }
    const std::wstring brand=Tr(L"豪之刃");
    Text(dc,brand,{tile.right+S(12),tile.top,SidebarWidth()-S(8),tile.top+S(22)},Display(UiTheme::Font::Heading,brand),UiTheme::Text(),DT_SINGLELINE|DT_LEFT|DT_VCENTER|DT_END_ELLIPSIS);
    UiHotkeys::Draw(dc,tile.right+S(12),tile.top+S(25),S(18),UiHotkeys::Get(UiHotkeys::Action::ToggleHub),SidebarWidth()-S(8));
    // Shortcut legend at the bottom of the sidebar, drawn as key caps.
    struct Hint { const wchar_t* keys[2]; const wchar_t* label; };
    const Hint hints[]={{{L"Esc"},L"隐藏面板"},{{L"Ctrl",L"Tab"},L"切换页面"},{{L"O",nullptr},L"游戏内原生菜单"}};
    int y=client.bottom-S(22)-static_cast<int>(std::size(hints))*S(30);
    const int legend_top=NavTop()+static_cast<int>(kPages)*NavStep()+S(8);
    if (y<legend_top) y=legend_top;
    RECT divider{S(22),y-S(14),SidebarWidth()-S(18),y-S(13)}; Fill(dc,divider,Mix(UiTheme::Border(),UiTheme::Base(),45));
    for (size_t hint_index=0;hint_index<std::size(hints);++hint_index) {
        const Hint& hint=hints[hint_index];
        int x=S(22);
        if (hint_index==0) x=UiHotkeys::Draw(dc,x,y+S(3),S(22),UiHotkeys::Get(UiHotkeys::Action::ToggleHub),SidebarWidth()-S(78));
        for (const wchar_t* key:hint.keys) {
            if (!key) continue;
            x=UiHotkeys::DrawKey(dc,x,y+S(3),S(22),key)+S(5);
        }
        Text(dc,Tr(hint.label),{x+S(4),y,SidebarWidth()-S(10),y+S(28)},Font(UiTheme::Font::Small),UiTheme::MutedText(),DT_SINGLELINE|DT_VCENTER|DT_LEFT|DT_END_ELLIPSIS);
        y+=S(30);
    }
    // Header: page title and subtitle slide in with each page change.
    const RECT card=CardRect();
    const double appear=title_.At(now);
    const int shift=static_cast<int>(std::lround((1.0-appear)*S(10)));
    const HWND first=pause_?pause_:segment_;
    RECT segment{}; if (first) { GetWindowRect(first,&segment); MapWindowPoints(nullptr,hwnd,reinterpret_cast<POINT*>(&segment),2); }
    const int right=first?segment.left-S(16):card.right;
    const std::wstring heading=Tr(entries[static_cast<size_t>(selected_)].title);
    Text(dc,heading,{card.left+S(8)+shift,S(14),right,S(46)},Display(UiTheme::Font::Title,heading),
        Mix(UiTheme::Base(),UiTheme::Text(),static_cast<int>(100*appear)),DT_SINGLELINE|DT_LEFT|DT_VCENTER|DT_END_ELLIPSIS);
    Text(dc,Tr(kSubtitles[selected_]),{card.left+S(8)+shift/2,S(46),right,S(68)},Font(UiTheme::Font::Small),
        Mix(UiTheme::Base(),UiTheme::MutedText(),static_cast<int>(100*appear)),DT_SINGLELINE|DT_LEFT|DT_VCENTER|DT_END_ELLIPSIS);
    // Content card that hosts the page viewport.
    Shape(dc,card,static_cast<float>(S(14)),UiTheme::Surface(),255,Mix(UiTheme::Border(),UiTheme::Base(),30));
    RECT sheen{card.left+S(14),card.top,card.right-S(14),card.top+1};
    Fill(dc,sheen,Mix(UiTheme::Border(),UiTheme::Surface(),15));
    // Hairline window outline, so the edge stays defined over bright scenes.
    // DWM draws it itself (in the same colour) when it rounds the corners.
    if (!UiGlass::GetSnapshot(hwnd).dwm_corners) Shape(dc,client,static_cast<float>(S(8)),0,0,Mix(UiTheme::Border(),UiTheme::Base(),20),255);
}
LRESULT HitTest(HWND hwnd,LPARAM lparam) {
    POINT point{GET_X_LPARAM(lparam),GET_Y_LPARAM(lparam)}; ScreenToClient(hwnd,&point);
    RECT client{}; GetClientRect(hwnd,&client);
    const int edge=S(7);
    if (!IsZoomed(hwnd)) {
        const bool left=point.x<edge,right=point.x>=client.right-edge,top=point.y<edge,bottom=point.y>=client.bottom-edge;
        if (top&&left) return HTTOPLEFT; if (top&&right) return HTTOPRIGHT;
        if (bottom&&left) return HTBOTTOMLEFT; if (bottom&&right) return HTBOTTOMRIGHT;
        if (left) return HTLEFT; if (right) return HTRIGHT; if (top) return HTTOP; if (bottom) return HTBOTTOM;
    }
    // Header strip and the brand block move the window.
    if (point.y<CardRect().top||(point.x<SidebarWidth()&&point.y<NavTop())) return HTCAPTION;
    return HTCLIENT;
}
void Layout() {
    if (!window_||!viewport_) return;
    RECT client{}; GetClientRect(window_,&client);
    if (nav_) SetWindowPos(nav_,nullptr,S(12),NavTop(),SidebarWidth()-S(24),static_cast<int>(kPages)*NavStep(),SWP_NOZORDER|SWP_NOACTIVATE);
    if (close_) SetWindowPos(close_,nullptr,client.right-S(14)-S(40),S(14),S(40),S(32),SWP_NOZORDER|SWP_NOACTIVATE);
    if (segment_) SetWindowPos(segment_,nullptr,client.right-S(14)-S(40)-S(12)-S(108),S(15),S(108),S(30),SWP_NOZORDER|SWP_NOACTIVATE);
    if (pause_) SetWindowPos(pause_,nullptr,client.right-S(14)-S(40)-S(12)-S(108)-S(10)-S(136),S(15),S(136),S(30),SWP_NOZORDER|SWP_NOACTIVATE);
    ActiveLayout(); InvalidateRect(window_,nullptr,FALSE);
}
void BeginFade(double target) {
    const ULONGLONG now=GetTickCount64();
    opacity_.ease=target>0?UiMotion::EaseOutCubic:UiMotion::EaseInCubic;
    rise_.ease=target>0?UiMotion::EaseOutQuint:UiMotion::EaseInCubic;
    opacity_.Set(target,target>0?220:150,now,reduced_);
    rise_.Set(target>0?0:S(6),target>0?320:150,now,reduced_);
}
LRESULT CALLBACK WindowProc(HWND hwnd,UINT message,WPARAM wparam,LPARAM lparam) {
    switch (message) {
    case WM_SETTINGCHANGE: case WM_THEMECHANGED: case WM_DWMCOMPOSITIONCHANGED: appearance_dirty_=true; break;
    case WM_NCCALCSIZE: if (wparam) return 0; break;
    case WM_NCPAINT: return 0;
    case WM_NCACTIVATE: return DefWindowProcW(hwnd,message,wparam,-1);
    case WM_NCHITTEST: return HitTest(hwnd,lparam);
    case WM_SETCURSOR:
        if (LOWORD(lparam)==HTCLIENT) { SetCursor(LoadCursorW(nullptr,IDC_ARROW)); return TRUE; }
        break;
    case WM_NCLBUTTONDBLCLK: if (wparam==HTCAPTION) return 0; break;
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: PaintHub(hwnd); return 0;
    case WM_CLOSE: Hide(); return 0;
    case WM_SIZE: Layout(); UiGlass::Sync(hwnd); return 0;
    case WM_MOVE: case WM_SHOWWINDOW:
        UiGlass::Sync(hwnd);
        if (message==WM_MOVE&&!moving_&&IsWindowVisible(hwnd)&&!rise_.Running(GetTickCount64())) {
            if (GetWindowLongPtrW(hwnd,GWL_STYLE)&WS_CHILD) {
                const int width=rest_.right-rest_.left,height=rest_.bottom-rest_.top;
                const int x=GET_X_LPARAM(lparam),y=GET_Y_LPARAM(lparam);
                if (width>0&&height>0) rest_={x,y,x+width,y+height};
            } else GetWindowRect(hwnd,&rest_);
        }
        break;
    case WM_ENTERSIZEMOVE: break;
    case WM_EXITSIZEMOVE: { RECT screen{}; GetWindowRect(hwnd,&screen); rest_=FromScreen(screen); break; }
    case WM_MOVING: case WM_SIZING: {
        auto* bounds=reinterpret_cast<RECT*>(lparam);
        const RECT client=UiHost::ClientScreen();
        if (client.right<=client.left||client.bottom<=client.top) break;
        const int width=bounds->right-bounds->left,height=bounds->bottom-bounds->top;
        if (bounds->left<client.left) { bounds->left=client.left; bounds->right=bounds->left+width; }
        if (bounds->top<client.top) { bounds->top=client.top; bounds->bottom=bounds->top+height; }
        if (bounds->right>client.right) { bounds->right=client.right; bounds->left=bounds->right-width; }
        if (bounds->bottom>client.bottom) { bounds->bottom=client.bottom; bounds->top=bounds->bottom-height; }
        if (bounds->left<client.left) bounds->left=client.left;
        if (bounds->top<client.top) bounds->top=client.top;
        return TRUE;
    }
    case WM_GETMINMAXINFO: {
        auto* info=reinterpret_cast<MINMAXINFO*>(lparam);
        const SIZE client=UiHost::ClientSize();
        // Same lower bound as the opening size: a larger minimum made Windows grow the
        // hub past the bottom of small or high-DPI game windows.
        int min_w=S(640),min_h=S(480);
        if (client.cx>0) min_w=(std::min)(min_w,static_cast<int>(client.cx));
        if (client.cy>0) min_h=(std::min)(min_h,static_cast<int>(client.cy));
        info->ptMinTrackSize={min_w,min_h};
        if (client.cx>0&&client.cy>0) { info->ptMaxTrackSize={client.cx,client.cy}; info->ptMaxSize={client.cx,client.cy}; }
        return 0;
    }
    case WM_DPICHANGED: {
        dpi_=HIWORD(wparam); if (dpi_<=0) dpi_=96;
        const RECT next=FromScreen(*reinterpret_cast<const RECT*>(lparam));
        SetWindowPos(hwnd,nullptr,next.left,next.top,next.right-next.left,next.bottom-next.top,SWP_NOZORDER|SWP_NOACTIVATE);
        rest_=next; Layout(); return 0;
    }
    case WM_COMMAND:
        if (LOWORD(wparam)==kLanguage&&HIWORD(wparam)==CBN_SELCHANGE) { SetLanguage(static_cast<int>(SendMessageW(segment_,CB_GETCURSEL,0,0))); return 0; }
        break;
    case WM_MOUSEWHEEL: Wheel(GET_WHEEL_DELTA_WPARAM(wparam),(GetKeyState(VK_SHIFT)&0x8000)!=0); return 0;
    case WM_DESTROY: window_=nav_=viewport_=segment_=close_=pause_=rails_[0]=rails_[1]=nullptr; return 0;
    default: break;
    }
    return DefWindowProcW(hwnd,message,wparam,lparam);
}
void Select(int index) {
    if (!window_||index<0||index>=static_cast<int>(kPages)) return;
    const bool changed=index!=selected_; const ULONGLONG now=GetTickCount64();
    selected_=index;
    const bool animate=changed&&IsWindowVisible(window_);
    pill_.ease=UiMotion::EaseOutCubic; pill_.Set(index,animate?360:0,now,reduced_);
    if (animate) {
        page_.ease=UiMotion::EaseOutCubic; page_.from=S(20); page_.to=0; page_.start=now; page_.duration=reduced_?0:320;
        title_.ease=UiMotion::EaseOutCubic; title_.from=0; title_.to=1; title_.start=now; title_.duration=reduced_?0:260;
    } else { page_.Jump(0); title_.Jump(1); }
    Entry& active=entries[static_cast<size_t>(index)];
    if (!active.window&&active.factory) {
        active.window=active.factory(viewport_);
        if (active.window) {
            RECT size{}; GetClientRect(active.window,&size);
            active.width=static_cast<int>(reinterpret_cast<INT_PTR>(GetPropW(active.window,kWidth)));
            active.height=static_cast<int>(reinterpret_cast<INT_PTR>(GetPropW(active.window,kHeight)));
            if (active.width<=0) active.width=size.right;
            if (active.height<=0) active.height=size.bottom;
        }
    }
    for (size_t i=0;i<entries.size();++i) if (entries[i].window) ShowWindow(entries[i].window,static_cast<int>(i)==index?SW_SHOW:SW_HIDE);
    ActiveLayout();
    InvalidateRect(window_,nullptr,FALSE); InvalidateRect(nav_,nullptr,FALSE);
    if (GetFocus()!=nav_) SetFocus(nav_);
}
bool Create() {
    if (window_) return true;
    const HINSTANCE instance=GetModuleHandleW(nullptr);
    WNDCLASSEXW cls{sizeof(cls)}; cls.hInstance=instance; cls.hCursor=LoadCursorW(nullptr,IDC_ARROW); cls.hbrBackground=nullptr;
    const std::pair<const wchar_t*,WNDPROC> classes[]={{kClass,WindowProc},{kViewport,ViewportProc},{kNav,NavProc},{kSegment,SegmentProc},{kClose,CloseProc},{kPause,PauseProc},{kRail,RailProc},{kToast,ToastProc}};
    for (const auto& item:classes) {
        cls.lpszClassName=item.first; cls.lpfnWndProc=item.second;
        if (!RegisterClassExW(&cls)&&GetLastError()!=ERROR_CLASS_ALREADY_EXISTS) return false;
    }
    // Open centred inside the game client. The window is parented below, so it
    // cannot sit on the desktop or above other applications.
    UiHost::Tick();
    const HWND anchor=UiHost::Game();
    const SIZE client=UiHost::ClientSize();
    const UINT anchor_dpi=anchor?GetDpiForWindow(anchor):0; dpi_=anchor_dpi?static_cast<int>(anchor_dpi):96;
    const int limit_w=client.cx>0?client.cx:1280,limit_h=client.cy>0?client.cy:900;
    int width=(std::min)(S(1280),(std::max)(S(640),limit_w-S(48)));
    int height=(std::min)(S(900),(std::max)(S(480),limit_h-S(48)));
    // The gear pill (GearWidget: 228x48 DIP, 28 DIP from the bottom-right corner)
    // shows while the hub is open; keep the hub out of that strip when it would reach it.
    if ((limit_w+width)/2>limit_w-S(256)) height=(std::min)(height,(std::max)(S(480),limit_h-S(176)));
    if (width>limit_w) width=limit_w;
    if (height>limit_h) height=limit_h;
    const int x=(std::max)(0,(limit_w-width)/2),y=(std::max)(0,(limit_h-height)/2);
    window_=CreateWindowExW(WS_EX_CONTROLPARENT,kClass,kTitle,
        WS_POPUP|WS_THICKFRAME|WS_CLIPCHILDREN,0,0,width,height,nullptr,nullptr,instance,nullptr);
    if (!window_) return false;
    const bool child=UiHost::Attach(window_);
    const UINT actual=GetDpiForWindow(window_); if (actual&&static_cast<int>(actual)!=dpi_) dpi_=static_cast<int>(actual);
    rest_={x,y,x+width,y+height};
    SetWindowPos(window_,nullptr,child?x:0,child?y:0,width,height,SWP_NOZORDER|SWP_NOACTIVATE|SWP_HIDEWINDOW);
    if (!child) rest_={0,0,width,height};
    nav_=CreateWindowExW(0,kNav,L"",WS_CHILD|WS_VISIBLE|WS_TABSTOP,0,0,1,1,window_,nullptr,instance,nullptr);
    viewport_=CreateWindowExW(WS_EX_CONTROLPARENT,kViewport,L"",WS_CHILD|WS_VISIBLE|WS_CLIPCHILDREN|WS_CLIPSIBLINGS,0,0,1,1,window_,nullptr,instance,nullptr);
    segment_=CreateWindowExW(0,kSegment,L"",WS_CHILD|WS_VISIBLE|WS_TABSTOP,0,0,1,1,window_,reinterpret_cast<HMENU>(static_cast<INT_PTR>(kLanguage)),instance,nullptr);
    close_=CreateWindowExW(0,kClose,L"",WS_CHILD|WS_VISIBLE,0,0,1,1,window_,reinterpret_cast<HMENU>(static_cast<INT_PTR>(kCloseId)),instance,nullptr);
    pause_=CreateWindowExW(0,kPause,L"",WS_CHILD|WS_VISIBLE|WS_TABSTOP,0,0,1,1,window_,nullptr,instance,nullptr);
    for (HWND& rail:rails_) rail=CreateWindowExW(0,kRail,L"",WS_CHILD,0,0,1,1,window_,nullptr,instance,nullptr);
    if (!nav_||!viewport_||!segment_||!close_||!pause_||!rails_[0]||!rails_[1]) { DestroyWindow(window_); return false; }
    knob_.Jump(UiI18n::GetLanguage()==UiI18n::Language::English?1:0); pause_knob_.Jump(HubPause::Enabled()?1:0); pause_hover_.Jump(0); pill_.Jump(selected_); title_.Jump(1); page_.Jump(0);
    opacity_.Jump(0); rise_.Jump(S(12));
    UiGlass::Apply(window_); UiGlass::SetOpacity(window_,0); applied_alpha_=0;
    const COLORREF edge=Mix(UiTheme::Border(),UiTheme::Base(),20); DwmSetWindowAttribute(window_,34 /*DWMWA_BORDER_COLOR*/,&edge,sizeof(edge));
    UiI18n::Bind(window_,kTitle);
    RefreshLanguage(); Layout(); return true;
}
}
void Initialize() { UiTheme::Initialize(); enabled_=true; reduced_=UiMotion::ReducedMotion(); }
void Register(Page page,const wchar_t* title,Factory factory) {
    const size_t index=static_cast<size_t>(page); if (index>=entries.size()) return;
    entries[index].title=title; entries[index].factory=std::move(factory);
}
bool Enabled() { return enabled_; }
void ShowPage(Page page) {
    if (enabled_) InterlockedExchange(&pending_page_,static_cast<LONG>(page));
}
void Toggle() { if (enabled_) InterlockedExchange(&pending_toggle_,1); }
void Hide() {
    if (!window_||hide_pending_) return;
    held_open_=false; parked_=false;
    const HWND foreground=GetForegroundWindow();
    restore_game_=foreground==window_;
    hide_pending_=true; ReturnFocus(); EnableWindow(window_,FALSE); BeginFade(0);
    if (restore_game_) if (const HWND game=CurrentGameWindow()) SetForegroundWindow(game);
    MotionTick();
}
namespace {
void MotionTick() {
    if (!window_) return;
    const ULONGLONG now=GetTickCount64(); const double dt=last_motion_?static_cast<double>(now-last_motion_):16.0; last_motion_=now;
    // Window fade and rise; the rest position is restored once hidden.
    const double alpha=opacity_.At(now),rise=rise_.At(now);
    const int byte=static_cast<int>(std::clamp(std::lround(alpha*255),0L,255L));
    if (byte!=applied_alpha_) { UiGlass::SetOpacity(window_,static_cast<BYTE>(byte)); applied_alpha_=byte; }
    RECT current{}; GetWindowRect(window_,&current);
    int current_top=current.top;
    if (ChildWindow()) current_top-=UiHost::ClientOrigin().y;
    const int wanted_top=rest_.top+static_cast<int>(std::lround(rise));
    if (IsWindowVisible(window_)&&!IsIconic(window_)&&current_top!=wanted_top&&(opacity_.Running(now)||rise_.Running(now))) {
        moving_=true; SetWindowPos(window_,nullptr,rest_.left,wanted_top,0,0,SWP_NOSIZE|SWP_NOZORDER|SWP_NOACTIVATE); moving_=false;
    }
    if (hide_pending_&&!opacity_.Running(now)) {
        ReturnFocus(); ShowWindow(window_,SW_HIDE); UiGlass::Hide(window_); hide_pending_=false;
        moving_=true; SetWindowPos(window_,nullptr,rest_.left,rest_.top,0,0,SWP_NOSIZE|SWP_NOZORDER|SWP_NOACTIVATE); moving_=false;
    }
    if (!IsWindowVisible(window_)) return;
    // Smooth scrolling: ease the page towards its scroll target.
    Entry& page=entries[static_cast<size_t>(selected_)];
    const double blend=reduced_?1.0:1.0-std::exp(-dt/70.0);
    bool scrolled=false;
    for (auto value:{std::make_pair(&page.x,page.target_x),std::make_pair(&page.y,page.target_y)}) {
        double& position=*value.first; const double goal=value.second;
        if (std::fabs(goal-position)<.5) { if (position!=goal) { position=goal; scrolled=true; } continue; }
        position+=(goal-position)*blend; scrolled=true;
    }
    static bool sliding=false;
    if (scrolled||page_.Running(now)||sliding) { PositionPage(); if (scrolled) for (HWND rail:rails_) InvalidateRect(rail,nullptr,FALSE); }
    sliding=page_.Running(now);
    if (title_.Running(now)) { RECT header{SidebarWidth(),0,CardRect().right,CardRect().top}; InvalidateRect(window_,&header,FALSE); }
    bool nav_moving=pill_.Running(now);
    for (const auto& hover:nav_hover_) nav_moving=nav_moving||hover.Running(now);
    static bool nav_was=false; if (nav_moving||nav_was) InvalidateRect(nav_,nullptr,FALSE); nav_was=nav_moving;
    static bool knob_was=false; if (knob_.Running(now)||knob_was) InvalidateRect(segment_,nullptr,FALSE); knob_was=knob_.Running(now);
    static bool close_was=false; if (close_hover_.Running(now)||close_was) InvalidateRect(close_,nullptr,FALSE); close_was=close_hover_.Running(now);
    const bool pause_moving=pause_knob_.Running(now)||pause_hover_.Running(now);
    static bool pause_was=false; if (pause_moving||pause_was) InvalidateRect(pause_,nullptr,FALSE); pause_was=pause_moving;
    for (int axis=0;axis<2;++axis) {
        static bool rail_was[2]{};
        const bool running=rail_state_[axis].width.Running(now);
        if (running||rail_was[axis]) InvalidateRect(rails_[axis],nullptr,FALSE);
        rail_was[axis]=running;
    }
}
}
bool ModifierDown() {
    return (GetKeyState(VK_CONTROL)&0x8000)||(GetKeyState(VK_MENU)&0x8000);
}
bool TextEntryFocused() {
    const HWND focus=GetFocus(); wchar_t name[64]{};
    if (!focus||GetClassNameW(focus,name,64)<=0) return false;
    return !_wcsicmp(name,L"Edit")||!_wcsnicmp(name,L"RichEdit",8)||!_wcsicmp(name,L"ComboBox")||!_wcsicmp(name,L"ComboBoxEx32");
}
void RequestNativeMenu() {
    InterlockedExchange64(&native_menu_deadline_,static_cast<LONG64>(GetTickCount64()+3000));
    InterlockedExchange(&native_menu_forward_,1);
}
void ReleaseTopmost() {
    if (window_) SetWindowPos(window_,HWND_NOTOPMOST,0,0,0,0,SWP_NOMOVE|SWP_NOSIZE|SWP_NOACTIVATE);
}
// The worker pump does not see UnrealWindow keys. Poll O only while this
// process's hub or game window is foreground, so a key typed elsewhere is ignored.
void PollNativeMenuKey() {
    const SHORT down=GetAsyncKeyState('O');
    const bool edge=(down&0x8000)&&!(native_o_down_&0x8000);
    native_o_down_=down;
    if (!edge||!window_||hide_pending_||!IsWindowVisible(window_)||ModifierDown()||TextEntryFocused()||UiHotkeys::Capturing()) return;
    const HWND foreground=GetForegroundWindow();
    const bool hub=HubFocused();
    const HWND root=foreground?GetAncestor(foreground,GA_ROOT):nullptr;
    const bool game=GameWindow(foreground)||GameWindow(root)||UiHost::InGame();
    if (!hub&&!game) return;
    // The game already received this O when focus is on its viewport. Hiding is enough.
    if (hub) RequestNativeMenu();
    ReleaseTopmost(); Hide();
}
void Tick() {
    if (!enabled_) return;
    const LONG request=InterlockedExchange(&pending_page_,-1),toggle=InterlockedExchange(&pending_toggle_,0);
    if (request>=0||toggle) {
        const bool visible=window_&&IsWindowVisible(window_);
        if (request<0&&visible&&!hide_pending_&&!parked_) Hide();
        else if (Create()&&UiHost::Attach(window_)) {
            const HWND previous=GetAncestor(GetForegroundWindow(),GA_ROOT);
            if (GameWindow(previous)) game_window_=previous;
            const bool was_visible=visible&&!hide_pending_&&!parked_;
            EnableWindow(window_,TRUE); hide_pending_=false; parked_=false; held_open_=true;
            if (!was_visible) {
                rise_.Jump(S(12)); moving_=true; SetWindowPos(window_,nullptr,rest_.left,rest_.top+static_cast<int>(std::lround(rise_.At(GetTickCount64()))),0,0,SWP_NOSIZE|SWP_NOZORDER|SWP_NOACTIVATE); moving_=false;
            }
            Select(request>=0?static_cast<int>(request):selected_);
            BeginFade(1);
            ShowWindow(window_,SW_SHOWNA);
            SetWindowPos(window_,HWND_TOP,0,0,0,0,SWP_NOMOVE|SWP_NOSIZE|SWP_NOACTIVATE);
            FocusHub();
            UiGlass::Sync(window_);
        }
    }
    MSG message{};
    while (PeekMessageW(&message,nullptr,0,0,PM_REMOVE)) {
        if (window_&&(message.hwnd==window_||IsChild(window_,message.hwnd))) {
            if (UiHotkeys::Capturing()&&(message.message==WM_KEYDOWN||message.message==WM_KEYUP||message.message==WM_SYSKEYDOWN||message.message==WM_SYSKEYUP||message.message==WM_CHAR)) continue;
            if (message.message==WM_KEYDOWN&&message.wParam==VK_ESCAPE) { if (!UiTheme::DismissPopup()) Hide(); continue; }
            if (message.message==WM_KEYDOWN&&(message.wParam=='O'||message.wParam=='o')&&!ModifierDown()&&!TextEntryFocused()) {
                if (!hide_pending_) { RequestNativeMenu(); ReleaseTopmost(); Hide(); }
                continue;
            }
            if (message.message==WM_KEYDOWN&&message.wParam==VK_TAB&&(GetKeyState(VK_CONTROL)&0x8000)) {
                const int direction=(GetKeyState(VK_SHIFT)&0x8000)?-1:1;
                Select((selected_+direction+static_cast<int>(entries.size()))%static_cast<int>(entries.size())); continue;
            }
            const HWND active=entries[static_cast<size_t>(selected_)].window;
            if (active&&(message.hwnd==active||IsChild(active,message.hwnd))&&IsDialogMessageW(active,&message)) continue;
            if (IsDialogMessageW(window_,&message)) continue;
        }
        TranslateMessage(&message); DispatchMessageW(&message);
    }
    if (appearance_dirty_&&window_) {
        appearance_dirty_=false; reduced_=UiMotion::ReducedMotion();
        for (Entry& entry:entries) if (entry.window) UiTheme::Apply(entry.window);
        UiGlass::Apply(window_); applied_alpha_=-1;
        InvalidateRect(window_,nullptr,FALSE);
    }
    if (layout_dirty_&&window_) { layout_dirty_=false; ActiveLayout(); }
    MotionTick(); ToastTick(GetTickCount64()); RevealFocus();
    if (!window_||!IsWindowVisible(window_)||hide_pending_||parked_) ReturnFocus();
    PollNativeMenuKey();
    UiHost::Tick();
    KeepInside();
    UiCursor::Sync(ObscuresGameplay());
    HubPause::Sync(ObscuresGameplay());
}
void Attach(HWND child,HWND parent,int min_width,int min_height) {
    if (!child||!parent) return;
    LONG_PTR style=GetWindowLongPtrW(child,GWL_STYLE);
    style&=~static_cast<LONG_PTR>(WS_POPUP|WS_CAPTION|WS_THICKFRAME|WS_SYSMENU|WS_MINIMIZEBOX|WS_MAXIMIZEBOX|WS_MINIMIZE|WS_MAXIMIZE);
    style|=WS_CHILD|WS_VISIBLE|WS_CLIPSIBLINGS|WS_CLIPCHILDREN;
    LONG_PTR extended=GetWindowLongPtrW(child,GWL_EXSTYLE);
    extended&=~static_cast<LONG_PTR>(WS_EX_APPWINDOW|WS_EX_TOOLWINDOW|WS_EX_TOPMOST|WS_EX_NOACTIVATE);
    extended|=WS_EX_CONTROLPARENT;
    SetWindowLongPtrW(child,GWL_STYLE,style); SetWindowLongPtrW(child,GWL_EXSTYLE,extended); SetParent(child,parent);
    SetPropW(child,kWidth,reinterpret_cast<HANDLE>(static_cast<INT_PTR>(min_width)));
    SetPropW(child,kHeight,reinterpret_cast<HANDLE>(static_cast<INT_PTR>(min_height)));
    SetWindowPos(child,HWND_NOTOPMOST,0,0,min_width,min_height,SWP_NOACTIVATE|SWP_FRAMECHANGED|SWP_SHOWWINDOW);
    wchar_t title[256]{}; GetWindowTextW(child,title,256); UiI18n::Bind(child,title);
    UiTheme::Apply(child); UiI18n::Apply(child);
}
bool Notify(const wchar_t* text) {
    if (!enabled_||!window_||!IsWindowVisible(window_)||hide_pending_||!text||!*text) return false;
    Toast& t=toast_;
    if (!t.window) {
        t.window=CreateWindowExW(WS_EX_LAYERED|WS_EX_NOACTIVATE,kToast,L"",WS_POPUP,0,0,1,1,nullptr,nullptr,GetModuleHandleW(nullptr),nullptr);
        if (t.window) UiHost::Attach(t.window);
    }
    if (!t.window) return false;
    const ULONGLONG now=GetTickCount64();
    const bool replace=t.active&&!t.leaving;
    t.text=text; t.shown=now; t.active=true; t.leaving=false; RenderToast();
    t.fade.ease=UiMotion::EaseOutCubic; t.slide.ease=UiMotion::EaseOutQuint;
    if (!replace) { t.fade.Jump(0); t.slide.Jump(S(16)); }
    t.fade.Set(1,200,now,reduced_); t.slide.Set(0,340,now,reduced_);
    ToastTick(now); ShowWindow(t.window,SW_SHOWNOACTIVATE);
    return true;
}
void SetMinimum(HWND page,int min_width,int min_height) {
    if (!page) return;
    SetPropW(page,kWidth,reinterpret_cast<HANDLE>(static_cast<INT_PTR>(min_width)));
    SetPropW(page,kHeight,reinterpret_cast<HANDLE>(static_cast<INT_PTR>(min_height)));
    for (Entry& entry:entries) if (entry.window==page&&(entry.width!=min_width||entry.height!=min_height)) {
        entry.width=min_width; entry.height=min_height; layout_dirty_=true;
    }
}
bool IsEmbedded(HWND child) { return child&&viewport_&&IsChild(viewport_,child); }
bool Visible() { return window_&&IsWindowVisible(window_)&&!parked_; }
bool ObscuresGameplay() { return window_&&IsWindowVisible(window_)&&!IsIconic(window_)&&!parked_; }
void RefreshChrome() {
    if (window_) InvalidateRect(window_,nullptr,FALSE);
    if (nav_) InvalidateRect(nav_,nullptr,FALSE);
}
bool TakeNativeMenuKey() {
    if (!InterlockedCompareExchange(&native_menu_forward_,0,0)) return false;
    const ULONGLONG now=GetTickCount64();
    const ULONGLONG deadline=static_cast<ULONGLONG>(InterlockedCompareExchange64(&native_menu_deadline_,0,0));
    if (now>deadline) { InterlockedExchange(&native_menu_forward_,0); return false; }
    if (!EntryFocus::GameplayFocused()) return false;
    return InterlockedExchange(&native_menu_forward_,0)!=0;
}
bool Animating() {
    const ULONGLONG now=GetTickCount64();
    if (opacity_.Running(now)||rise_.Running(now)||page_.Running(now)||title_.Running(now)||pill_.Running(now)||knob_.Running(now)||close_hover_.Running(now)||pause_knob_.Running(now)||pause_hover_.Running(now)) return true;
    for (const auto& hover:nav_hover_) if (hover.Running(now)) return true;
    for (const Rail& rail:rail_state_) if (rail.width.Running(now)) return true;
    if (toast_.active) return true;
    const Entry& page=entries[static_cast<size_t>(selected_)];
    return std::fabs(page.x-page.target_x)>=.5||std::fabs(page.y-page.target_y)>=.5;
}
void Shutdown() {
    UiCursor::Sync(false); HubPause::Sync(false);
    enabled_=false; held_open_=false; parked_=false;
    if (toast_.window&&IsWindow(toast_.window)) { UiHost::Detach(toast_.window); DestroyWindow(toast_.window); }
    if (window_) { UiHost::Detach(window_); DestroyWindow(window_); }
    toast_.window=nullptr; toast_.active=false; toast_.surface.Reset();
    hide_pending_=false; opacity_.Jump(0); rise_.Jump(0);
    for (Entry& entry:entries) entry={};
}
}
