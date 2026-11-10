#pragma once
#include "ui_theme.h"
#include "bone_editor_skin.h"
#include <windowsx.h>
#include <algorithm>
#include <cmath>
#include <cwchar>

// Original GDI slider: native EDIT remains the precise, accessible text input.
// Preview notifications never persist the global configuration; release commits once.
namespace BoneSlider {
constexpr int Preview=0x501,Commit=0x502;
struct State { HWND field=nullptr;double lo=0,hi=1,step=.01,initial=0;bool drag=false; };
inline double Value(const State& s){wchar_t b[64]{};GetWindowTextW(s.field,b,64);const double v=wcstod(b,nullptr);return std::isfinite(v)?std::clamp(v,s.lo,s.hi):s.lo;}
inline void Set(HWND w,State& s,double v,int notification){v=std::clamp(v,s.lo,s.hi);wchar_t b[64]{};swprintf_s(b,L"%.3f",v);SetWindowTextW(s.field,b);InvalidateRect(w,nullptr,FALSE);SendMessageW(GetParent(w),WM_COMMAND,MAKEWPARAM(GetDlgCtrlID(w),notification),reinterpret_cast<LPARAM>(w));}
inline void Pointer(HWND w,State& s,int x,int notification){RECT r{};GetClientRect(w,&r);const double f=std::clamp((x-8.0)/(std::max)(1L,r.right-16),0.0,1.0);Set(w,s,s.lo+f*(s.hi-s.lo),notification);}
inline LRESULT CALLBACK Proc(HWND w,UINT msg,WPARAM wp,LPARAM lp){auto* s=reinterpret_cast<State*>(GetWindowLongPtrW(w,GWLP_USERDATA));
    if(msg==WM_NCCREATE){s=new State(*static_cast<State*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams));SetWindowLongPtrW(w,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(s));}
    if(!s)return DefWindowProcW(w,msg,wp,lp);
    if(msg==WM_NCDESTROY){delete s;SetWindowLongPtrW(w,GWLP_USERDATA,0);return DefWindowProcW(w,msg,wp,lp);}
    if(msg==WM_ERASEBKGND)return 1;
    if(msg==WM_ENABLE||msg==WM_SETFOCUS||msg==WM_KILLFOCUS){InvalidateRect(w,nullptr,FALSE);return 0;}
    if(msg==WM_PAINT||msg==WM_PRINTCLIENT){PAINTSTRUCT ps{};HDC dc=msg==WM_PAINT?BeginPaint(w,&ps):reinterpret_cast<HDC>(wp);RECT r{};GetClientRect(w,&r);FillRect(dc,&r,UiTheme::PanelBrush());
        const bool enabled=IsWindowEnabled(w)!=FALSE;const int x=8+static_cast<int>((r.right-16)*(Value(*s)-s->lo)/(s->hi-s->lo));
        BoneEditorSkin::Fill(dc,r,BoneEditorSkin::Surface());
        RECT knob{x-4,3,x+4,r.bottom-3};BoneEditorSkin::Fill(dc,knob,enabled?BoneEditorSkin::Accent():BoneEditorSkin::Muted());
        const int id=GetDlgCtrlID(w);const bool scale=(id>=860&&id<=862)||(id>=886&&id<=888)||id==894||id==895;
        wchar_t label[64]{};swprintf_s(label,L"%.3f%ls",Value(*s),scale?L"×":id>=883&&id<=885?L"°":L"");
        auto oldFont=SelectObject(dc,BoneEditorSkin::Font(w));BoneEditorSkin::Label(dc,r,label,enabled?BoneEditorSkin::Text():BoneEditorSkin::Muted(),DT_CENTER|DT_SINGLELINE|DT_VCENTER);SelectObject(dc,oldFont);
        if(GetFocus()==w){InflateRect(&r,-1,-1);DrawFocusRect(dc,&r);}if(msg==WM_PAINT)EndPaint(w,&ps);return 0;}
    if(msg==WM_GETDLGCODE)return DLGC_WANTARROWS;
    if(msg==WM_LBUTTONDOWN&&IsWindowEnabled(w)){s->initial=Value(*s);s->drag=true;if(IsWindowVisible(w)){SetFocus(w);SetCapture(w);}Pointer(w,*s,GET_X_LPARAM(lp),Preview);return 0;}
    if(msg==WM_MOUSEMOVE&&s->drag){Pointer(w,*s,GET_X_LPARAM(lp),Preview);return 0;}
    if(msg==WM_LBUTTONUP&&s->drag){s->drag=false;if(GetCapture()==w)ReleaseCapture();Pointer(w,*s,GET_X_LPARAM(lp),Commit);return 0;}
    if(msg==WM_CAPTURECHANGED&&s->drag){s->drag=false;Set(w,*s,Value(*s),Commit);return 0;}
    if(msg==WM_KEYDOWN&&IsWindowEnabled(w)){
        if(wp==VK_ESCAPE&&s->drag){s->drag=false;if(GetCapture()==w)ReleaseCapture();Set(w,*s,s->initial,Commit);return 0;}
        if(wp==VK_LEFT||wp==VK_DOWN||wp==VK_RIGHT||wp==VK_UP||wp==VK_HOME||wp==VK_END){const double step=s->step*((GetKeyState(VK_SHIFT)&0x8000)?.1:1);Set(w,*s,wp==VK_HOME?s->lo:wp==VK_END?s->hi:Value(*s)+((wp==VK_LEFT||wp==VK_DOWN)?-step:step),Commit);return 0;}}
    if(msg==WM_MOUSEWHEEL&&IsWindowEnabled(w)){Set(w,*s,Value(*s)+GET_WHEEL_DELTA_WPARAM(wp)/120.0*s->step,Commit);return 0;}
    return DefWindowProcW(w,msg,wp,lp);
}
inline HWND Create(HWND parent,int id,HWND field,double lo,double hi,double step){WNDCLASSW c{};c.lpfnWndProc=Proc;c.hInstance=GetModuleHandleW(nullptr);c.lpszClassName=L"PBZBoneSlider";c.hCursor=LoadCursorW(nullptr,IDC_HAND);RegisterClassW(&c);State s{field,lo,hi,step};return CreateWindowExW(0,c.lpszClassName,L"",WS_CHILD|WS_VISIBLE|WS_TABSTOP,0,0,10,10,parent,reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),c.hInstance,&s);}
}
