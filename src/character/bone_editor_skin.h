#pragma once
#include <windows.h>
#include "ui_theme.h"
#include <commctrl.h>
#include <map>
#include <string>

// Independent flat GDI styling scoped to the bone editor. No upstream code or assets.
namespace BoneEditorSkin {
inline COLORREF Background(){return UiTheme::Base();}
inline COLORREF Surface(){return UiTheme::Panel();}
inline COLORREF Section(){return UiTheme::Selection();}
inline COLORREF Accent(){return UiTheme::Accent();}
inline COLORREF Text(){return UiTheme::Text();}
inline COLORREF Muted(){return UiTheme::MutedText();}
inline std::map<UINT,HFONT> fonts;
inline HFONT Font(HWND w){UINT dpi=GetDpiForWindow(w);if(!dpi)dpi=96;auto& f=fonts[dpi];if(!f)f=CreateFontW(-MulDiv(15,static_cast<int>(dpi),96),0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH,L"Segoe UI");return f;}
inline HBRUSH Brush(){static HBRUSH b=CreateSolidBrush(Background());return b;}
inline void Fill(HDC dc,RECT r,COLORREF color){HBRUSH b=CreateSolidBrush(color);FillRect(dc,&r,b);DeleteObject(b);}
inline std::wstring Caption(HWND w){const int n=GetWindowTextLengthW(w);std::wstring t(static_cast<size_t>(n)+1,L'\0');GetWindowTextW(w,t.data(),n+1);t.resize(static_cast<size_t>(n));return t;}
inline void Label(HDC dc,RECT r,const std::wstring& t,COLORREF color,UINT flags=DT_LEFT|DT_SINGLELINE|DT_VCENTER|DT_END_ELLIPSIS){SetBkMode(dc,TRANSPARENT);SetTextColor(dc,color);DrawTextW(dc,t.c_str(),-1,&r,flags|DT_NOPREFIX);}
inline LRESULT CALLBACK Proc(HWND w,UINT msg,WPARAM wp,LPARAM lp,UINT_PTR,DWORD_PTR kind){
    if(msg==WM_CTLCOLORSTATIC||msg==WM_CTLCOLOREDIT||msg==WM_CTLCOLORLISTBOX||msg==WM_CTLCOLORBTN){SetTextColor(reinterpret_cast<HDC>(wp),Text());SetBkColor(reinterpret_cast<HDC>(wp),Background());return reinterpret_cast<LRESULT>(Brush());}
    if(msg==WM_ERASEBKGND&&kind&&kind!=4)return 1;
    if(msg==WM_ERASEBKGND&&kind==0){RECT r{};GetClientRect(w,&r);Fill(reinterpret_cast<HDC>(wp),r,Background());return 1;}
    if((msg==WM_PAINT||msg==WM_PRINTCLIENT||(msg==WM_PRINT&&kind==4))&&kind&&!(kind==4&&msg==WM_PAINT)){
        PAINTSTRUCT ps{};HDC target=msg==WM_PAINT?BeginPaint(w,&ps):reinterpret_cast<HDC>(wp);RECT r{};GetClientRect(w,&r);HDC dc=CreateCompatibleDC(target);HBITMAP bitmap=CreateCompatibleBitmap(target,(std::max)(1L,r.right),(std::max)(1L,r.bottom));auto oldBitmap=SelectObject(dc,bitmap);auto font=SelectObject(dc,Font(w));const auto t=Caption(w);const bool enabled=IsWindowEnabled(w)!=FALSE;
        if(kind==1){const auto style=GetWindowLongPtrW(w,GWL_STYLE)&BS_TYPEMASK;const bool check=style==BS_AUTOCHECKBOX||style==BS_CHECKBOX;
            const bool selected=GetPropW(w,L"PBZBoneSelected")!=nullptr,section=GetPropW(w,L"PBZBoneSection")!=nullptr;
            Fill(dc,r,check?Background():selected||section?Section():Surface());RECT label=r;label.left+=8;label.right-=8;
            if(check){RECT box{3,(r.bottom-17)/2,20,(r.bottom+17)/2};Fill(dc,box,Surface());if(SendMessageW(w,BM_GETCHECK,0,0)==BST_CHECKED){InflateRect(&box,-3,-3);Fill(dc,box,Accent());}label.left=27;}
            Label(dc,label,t,enabled?Text():Muted(),DT_VCENTER|DT_SINGLELINE|DT_END_ELLIPSIS|(check||section?DT_LEFT:DT_CENTER));
            if(GetFocus()==w){InflateRect(&r,-1,-1);DrawFocusRect(dc,&r);}
        }else if(kind==2){Fill(dc,r,Background());Label(dc,r,t,GetPropW(w,L"PBZBoneMuted")?Muted():Text(),DT_LEFT|DT_WORDBREAK);}
        else if(kind==3){Fill(dc,r,Background());const LRESULT n=SendMessageW(w,LB_GETCOUNT,0,0),selected=SendMessageW(w,LB_GETCURSEL,0,0);
            for(LRESULT i=SendMessageW(w,LB_GETTOPINDEX,0,0);i>=0&&i<n;++i){RECT item{};if(SendMessageW(w,LB_GETITEMRECT,i,reinterpret_cast<LPARAM>(&item))==LB_ERR||item.top>=r.bottom)break;
                const LRESULT len=SendMessageW(w,LB_GETTEXTLEN,i,0);if(len<0||len>1024)continue;std::wstring line(static_cast<size_t>(len)+1,L'\0');SendMessageW(w,LB_GETTEXT,i,reinterpret_cast<LPARAM>(line.data()));line.resize(static_cast<size_t>(len));
                if(i==selected)Fill(dc,item,Section());item.left+=10;Label(dc,item,line,i==selected?Accent():Text());}}
        else if(kind==4){Fill(dc,r,Surface());RECT label=r;label.left+=7;label.right-=7;std::wstring value=t;if(value.empty()&&GetDlgCtrlID(w)==811)value=L"搜索骨骼…";Label(dc,label,value,t.empty()?Muted():Text());}
        else if(kind==5){Fill(dc,r,Surface());RECT label=r;label.left+=7;label.right-=26;Label(dc,label,t,enabled?Text():Muted());label={r.right-23,0,r.right,r.bottom};Label(dc,label,L"▾",Muted(),DT_CENTER|DT_VCENTER|DT_SINGLELINE);}
        SelectObject(dc,font);BitBlt(target,0,0,r.right,r.bottom,dc,0,0,SRCCOPY);SelectObject(dc,oldBitmap);DeleteObject(bitmap);DeleteDC(dc);if(msg==WM_PAINT)EndPaint(w,&ps);return 0;
    }
    if(msg==WM_SETTEXT||msg==BM_SETCHECK||msg==CB_SETCURSEL||msg==WM_ENABLE){auto result=DefSubclassProc(w,msg,wp,lp);InvalidateRect(w,nullptr,FALSE);return result;}
    return DefSubclassProc(w,msg,wp,lp);
}
inline void Apply(HWND root){SetWindowSubclass(root,Proc,211,0);EnumChildWindows(root,[](HWND w,LPARAM)->BOOL{wchar_t cls[64]{};GetClassNameW(w,cls,64);DWORD_PTR kind=0;
    if(!_wcsicmp(cls,L"Button"))kind=1;else if(!_wcsicmp(cls,L"Static"))kind=2;else if(!_wcsicmp(cls,L"ListBox"))kind=3;else if(!_wcsicmp(cls,L"Edit"))kind=4;else if(!_wcsicmp(cls,L"ComboBox"))kind=5;
    SendMessageW(w,WM_SETFONT,reinterpret_cast<WPARAM>(Font(w)),FALSE);if(kind)SetWindowSubclass(w,Proc,211,kind);return TRUE;},0);}
inline void Selected(HWND w,bool value){if((GetPropW(w,L"PBZBoneSelected")!=nullptr)==value)return;if(value)SetPropW(w,L"PBZBoneSelected",reinterpret_cast<HANDLE>(1));else RemovePropW(w,L"PBZBoneSelected");InvalidateRect(w,nullptr,FALSE);}
inline void Shutdown(){for(auto& pair:fonts)DeleteObject(pair.second);fonts.clear();}
}
