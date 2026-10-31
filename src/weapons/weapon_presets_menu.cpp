#include "weapon_presets_menu.h"
#include "weapon_presets.h"
#include "weapon_icons.h"
#include "debug_hub.h"
#include "ui_assets.h"
#include "ui_i18n.h"
#include "ui_layout.h"
#include "ui_theme.h"
#include <objidl.h>
#include <gdiplus.h>
#include <algorithm>
#include <cwchar>
#include <string>
#include <vector>

namespace WeaponPresetsMenu {
namespace {
constexpr wchar_t kClass[]=L"PBZWeaponPresetsPage_v1",kLoadoutClass[]=L"PBZWeaponLoadout_v1";
constexpr int kEnable=101,kSwap=102,kRead=103,kSave1=104,kSave2=105,kUse1=106,kUse2=107;
HWND window_=nullptr,enable_=nullptr,swap_=nullptr,read_=nullptr,intro_=nullptr;
HWND status_=nullptr,touch_=nullptr,operation_heading_=nullptr,heading_[2]{},set_[2]{};
HWND live_heading_=nullptr,live_=nullptr,read_status_=nullptr,save_[2]{},use_[2]{};
ULONGLONG next_=0;
HWND Control(const wchar_t* cls,const wchar_t* text,DWORD style,int id=0) {
    const HWND control=CreateWindowExW(0,cls,text,WS_CHILD|WS_VISIBLE|style,0,0,10,10,window_,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),GetModuleHandleW(nullptr),nullptr);
    if (control) {
        SendMessageW(control,WM_SETFONT,reinterpret_cast<WPARAM>(UiTheme::GetFont(UiTheme::Font::Body,window_)),FALSE);
        UiI18n::Bind(control,text);
    }
    return control;
}
std::wstring Tr(const wchar_t* text) { return UiI18n::Translate(text); }

// ---- loadout view -------------------------------------------------------------------
// Weapon cards for one preset: the game's HUD icon, its display name and which
// slot is in use. Main weapons first, then Shachi weapons.
struct Loadout { WeaponPresets::PresetView view; std::wstring empty; UiI18n::Language language=UiI18n::Language::SimplifiedChinese; };
struct Geometry { int caption,tile,gap,minimum,note; };
Geometry Measure(HWND window) {
    const auto m=UiLayout::For(window);
    return {m.S(24),m.S(58),m.S(8),m.S(196),m.S(22)};
}
Loadout* Data(HWND window) { return reinterpret_cast<Loadout*>(GetWindowLongPtrW(window,GWLP_USERDATA)); }
int Columns(const Geometry& g,int width,size_t count) {
    const int fit=(std::max)(1,(width+g.gap)/(g.minimum+g.gap));
    return (std::max)(1,(std::min)(fit,static_cast<int>((std::max)(count,static_cast<size_t>(1)))));
}
int GroupHeight(const Geometry& g,int width,size_t count) {
    const int columns=Columns(g,width,count),rows=static_cast<int>((std::max)(count,static_cast<size_t>(1))+columns-1)/columns;
    return g.caption+rows*(g.tile+g.gap);
}
int LoadoutHeight(HWND window,int width) {
    const Loadout* data=window?Data(window):nullptr;
    const Geometry g=Measure(window);
    if (!data||!data->view.known) return g.caption*2;
    int height=GroupHeight(g,width,data->view.main_weapons.size());
    if (data->view.special_slot) height+=g.note;
    return height+GroupHeight(g,width,data->view.shaqi_weapons.size())-g.gap;
}
bool SameView(const WeaponPresets::PresetView& a,const WeaponPresets::PresetView& b) {
    return a.known==b.known&&a.special_slot==b.special_slot&&a.main_index==b.main_index&&a.shaqi_index==b.shaqi_index&&
        a.main_weapons==b.main_weapons&&a.shaqi_weapons==b.shaqi_weapons;
}
void SetLoadout(HWND window,const WeaponPresets::PresetView& view,const wchar_t* empty) {
    Loadout* data=window?Data(window):nullptr;
    if (!data) return;
    const UiI18n::Language language=UiI18n::GetLanguage();
    if (SameView(data->view,view)&&data->empty==empty&&data->language==language) return;
    data->view=view; data->empty=empty; data->language=language;
    InvalidateRect(window,nullptr,FALSE);
}
Gdiplus::Color Argb(COLORREF color,BYTE alpha=255) { return Gdiplus::Color(alpha,GetRValue(color),GetGValue(color),GetBValue(color)); }
void Rounded(Gdiplus::Graphics& graphics,const RECT& rect,float radius,COLORREF fill,COLORREF edge) {
    const float x=static_cast<float>(rect.left)+.5f,y=static_cast<float>(rect.top)+.5f;
    const float w=static_cast<float>(rect.right-rect.left)-1.f,h=static_cast<float>(rect.bottom-rect.top)-1.f,d=radius*2;
    Gdiplus::GraphicsPath path;
    path.AddArc(x,y,d,d,180,90); path.AddArc(x+w-d,y,d,d,270,90); path.AddArc(x+w-d,y+h-d,d,d,0,90); path.AddArc(x,y+h-d,d,d,90,90); path.CloseFigure();
    Gdiplus::SolidBrush brush(Argb(fill)); graphics.FillPath(&brush,&path);
    Gdiplus::Pen pen(Argb(edge),1.f); graphics.DrawPath(&pen,&path);
}
void Label(HDC dc,const std::wstring& text,RECT rect,HFONT font,COLORREF color,UINT flags) {
    const HGDIOBJ old=SelectObject(dc,font); SetTextColor(dc,color);
    DrawTextW(dc,text.c_str(),-1,&rect,flags|DT_NOPREFIX); SelectObject(dc,old);
}
void PaintGroup(HDC dc,Gdiplus::Graphics& graphics,HWND window,const Geometry& g,int width,int& y,const wchar_t* caption,
    const std::vector<std::wstring>& weapons,int32_t selected,bool shaqi) {
    const auto m=UiLayout::For(window);
    UiAssets::Draw(graphics,UiAssets::Id::GoldArrow,Gdiplus::RectF(0.f,static_cast<float>(y+(g.caption-m.S(12))/2),static_cast<float>(m.S(12)),static_cast<float>(m.S(12))));
    Label(dc,Tr(caption),{m.S(18),y,width,y+g.caption},UiTheme::GetFont(UiTheme::Font::Small,window),UiTheme::MutedText(),DT_SINGLELINE|DT_VCENTER|DT_LEFT|DT_END_ELLIPSIS);
    y+=g.caption;
    const int columns=Columns(g,width,weapons.size());
    const int tile_w=(width-(columns-1)*g.gap)/columns;
    const COLORREF paper=UiTheme::Mix(UiTheme::Base(),UiTheme::Panel(),72),edge=UiTheme::Mix(UiTheme::Border(),UiTheme::Base(),15);
    if (weapons.empty()) {
        Label(dc,L"\x2014",{m.S(18),y,width,y+g.tile},UiTheme::GetFont(UiTheme::Font::Body,window),UiTheme::MutedText(),DT_SINGLELINE|DT_VCENTER|DT_LEFT);
        y+=g.tile+g.gap; return;
    }
    for (size_t i=0;i<weapons.size();++i) {
        const int column=static_cast<int>(i)%columns,row=static_cast<int>(i)/columns;
        const RECT tile{column*(tile_w+g.gap),y+row*(g.tile+g.gap),column*(tile_w+g.gap)+tile_w,y+row*(g.tile+g.gap)+g.tile};
        const bool on=static_cast<int32_t>(i)==selected;
        Rounded(graphics,tile,static_cast<float>(m.S(8)),on?UiTheme::Mix(paper,UiTheme::Accent(),8):paper,on?UiTheme::Mix(edge,UiTheme::Accent(),75):edge);
        const WeaponIcons::Info info=WeaponIcons::Find(weapons[i].c_str(),shaqi);
        const RECT icon{tile.left+m.S(8),tile.top+m.S(6),tile.left+m.S(8)+m.S(92),tile.bottom-m.S(6)};
        if (info.image>=0) WeaponIcons::Draw(graphics,info.image,Gdiplus::RectF(static_cast<float>(icon.left),static_cast<float>(icon.top),
            static_cast<float>(icon.right-icon.left),static_cast<float>(icon.bottom-icon.top)),on?1.f:.82f);
        else Label(dc,L"?",icon,UiTheme::GetFont(UiTheme::Font::Strong,window),UiTheme::MutedText(),DT_SINGLELINE|DT_VCENTER|DT_CENTER);
        const int text_left=icon.right+m.S(10),middle=(tile.top+tile.bottom)/2;
        const std::wstring name=info.name?info.name:weapons[i];
        Label(dc,name,{text_left,tile.top+m.S(6),tile.right-m.S(10),middle+m.S(2)},UiTheme::GetFont(on?UiTheme::Font::Strong:UiTheme::Font::Body,window),
            on?UiTheme::Text():UiTheme::Mix(UiTheme::MutedText(),UiTheme::Text(),70),DT_SINGLELINE|DT_BOTTOM|DT_LEFT|DT_END_ELLIPSIS);
        wchar_t slot[64]{}; swprintf_s(slot,Tr(L"槽位 %d").c_str(),static_cast<int>(i)+1);
        Label(dc,on?Tr(L"使用中"):std::wstring(slot),{text_left,middle+m.S(3),tile.right-m.S(10),tile.bottom-m.S(4)},UiTheme::GetFont(UiTheme::Font::Small,window),
            on?UiTheme::Accent():UiTheme::MutedText(),DT_SINGLELINE|DT_TOP|DT_LEFT|DT_END_ELLIPSIS);
    }
    y+=static_cast<int>((weapons.size()+static_cast<size_t>(columns)-1)/static_cast<size_t>(columns))*(g.tile+g.gap);
}
void PaintLoadout(HWND window) {
    PAINTSTRUCT paint{}; const HDC target=BeginPaint(window,&paint);
    RECT client{}; GetClientRect(window,&client);
    const int width=(std::max)(1L,client.right),height=(std::max)(1L,client.bottom);
    const HDC dc=CreateCompatibleDC(target);
    const HBITMAP bitmap=dc?CreateCompatibleBitmap(target,width,height):nullptr;
    const HGDIOBJ old=bitmap?SelectObject(dc,bitmap):nullptr;
    const HDC draw=bitmap?dc:target;
    // Off-screen bitmaps start black: fill the page colour before anything round is drawn.
    FillRect(draw,&client,UiTheme::Background()); SetBkMode(draw,TRANSPARENT);
    if (const Loadout* data=Data(window)) {
        Gdiplus::Graphics graphics(draw);
        graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        const Geometry g=Measure(window);
        if (!data->view.known) {
            RECT text{0,0,width,height};
            Label(draw,Tr(data->empty.c_str()),text,UiTheme::GetFont(UiTheme::Font::Body,window),UiTheme::MutedText(),DT_WORDBREAK|DT_LEFT|DT_TOP);
        } else {
            int y=0;
            PaintGroup(draw,graphics,window,g,width,y,L"主武器",data->view.main_weapons,data->view.special_slot?-1:data->view.main_index,false);
            if (data->view.special_slot) {
                std::wstring note=Tr(L"  > 正在使用游戏的特殊主武器槽；回到普通武器后可保存或切换预设。");
                note.erase(0,note.find_first_not_of(L" >"));
                Label(draw,note,{0,y-g.gap,width,y-g.gap+g.note},UiTheme::GetFont(UiTheme::Font::Small,window),UiTheme::Accent(),DT_SINGLELINE|DT_VCENTER|DT_LEFT|DT_END_ELLIPSIS);
                y+=g.note;
            }
            PaintGroup(draw,graphics,window,g,width,y,L"杀气武器",data->view.shaqi_weapons,data->view.shaqi_index,true);
        }
    }
    if (bitmap) { BitBlt(target,0,0,width,height,dc,0,0,SRCCOPY); SelectObject(dc,old); DeleteObject(bitmap); }
    if (dc) DeleteDC(dc);
    EndPaint(window,&paint);
}
LRESULT CALLBACK LoadoutProc(HWND window,UINT message,WPARAM wparam,LPARAM lparam) {
    switch (message) {
    case WM_NCCREATE: SetWindowLongPtrW(window,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(new Loadout)); break;
    case WM_NCDESTROY: delete Data(window); SetWindowLongPtrW(window,GWLP_USERDATA,0); break;
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: PaintLoadout(window); return 0;
    case WM_SIZE: InvalidateRect(window,nullptr,FALSE); return 0;
    default: break;
    }
    return DefWindowProcW(window,message,wparam,lparam);
}

void Refresh() {
    if (!window_) return;
    const auto state=WeaponPresets::GetSnapshot();
    SendMessageW(enable_,BM_SETCHECK,state.enabled?BST_CHECKED:BST_UNCHECKED,0);
    EnableWindow(swap_,state.enabled&&state.ready&&state.can_save&&!state.busy);
    EnableWindow(read_,!state.busy);
    SetLoadout(live_,state.current,L"当前实际装备读取尚未就绪；已保存预设不会被旧数据覆盖。");
    UiI18n::SetText(read_status_,state.read_status.c_str());
    std::wstring status=Tr(state.status.c_str());
    status+=L"\r\n"+Tr(L"当前预设")+L" "+std::to_wstring(state.current_set+1)+L"  ·  "+Tr(L"切换次数")+L" "+std::to_wstring(state.swaps);
    UiI18n::SetText(status_,status.c_str());
    UiI18n::SetText(touch_,state.touchpad_status.c_str());
    for (int i=0;i<2;++i) {
        EnableWindow(save_[i],state.current.known&&state.can_save&&!state.busy);
        EnableWindow(use_[i],state.enabled&&state.ready&&state.can_save&&!state.busy&&state.sets[i].known);
        const std::wstring title=Tr(i==0?L"预设一 / 原始装备":L"预设二")+(state.current_set==i&&state.enabled?L"  ·  "+Tr(L"当前"):L"");
        UiI18n::SetText(heading_[i],title.c_str()); SetLoadout(set_[i],state.sets[i],L"尚未记忆");
    }
}
void Layout() {
    if (!window_||!intro_) return;
    RECT rect{}; GetClientRect(window_,&rect); const auto m=UiLayout::For(window_);
    const int x=m.Margin(),gap=m.Gap(),w=(std::max)(m.S(620),static_cast<int>(rect.right)-2*x);
    int y=x;
    UiLayout::Place(enable_,x,y,(std::min)(w,UiLayout::CheckWidth(enable_,m)),m.Check()); y+=m.Check()+gap;
    const int intro_height=UiLayout::TextHeight(intro_,w);
    UiLayout::Place(intro_,x,y,w,intro_height); y+=intro_height+gap*2;
    y=UiLayout::ButtonRow({read_,swap_},x,y,w,m,m.S(150))+gap;
    UiLayout::Place(live_heading_,x,y,w,m.Section()); y+=m.Section()+gap/2;
    const int live_height=LoadoutHeight(live_,w),read_height=UiLayout::TextHeight(read_status_,w,1);
    UiLayout::Place(live_,x,y,w,live_height); y+=live_height+gap/2;
    UiLayout::Place(read_status_,x,y,w,read_height); y+=read_height+gap;
    UiLayout::Place(operation_heading_,x,y,w,m.Section()); y+=m.Section()+gap/2;
    const int status_height=UiLayout::TextHeight(status_,w,3),touch_height=UiLayout::TextHeight(touch_,w,2);
    UiLayout::Place(status_,x,y,w,status_height); y+=status_height+gap;
    UiLayout::Place(touch_,x,y,w,touch_height); y+=touch_height+gap*2;
    const int width=(w-gap)/2;
    int top=y+m.Section()+gap/2;
    for (int i=0;i<2;++i) {
        UiLayout::Place(heading_[i],x+i*(width+gap),y,width,m.Section());
        const int bottom=UiLayout::ButtonRow({save_[i],use_[i]},x+i*(width+gap),y+m.Section()+gap/2,width,m,m.S(125));
        top=(std::max)(top,bottom+gap);
    }
    const int height=(std::max)(LoadoutHeight(set_[0],width),LoadoutHeight(set_[1],width));
    for (int i=0;i<2;++i) UiLayout::Place(set_[i],x+i*(width+gap),top,width,height);
    DebugHub::SetMinimum(window_,m.S(620)+2*x,top+height+x);
}
void Rejected() { DebugHub::Notify(Tr(L"请求未接受，请等待当前操作完成后重试。").c_str()); }
LRESULT CALLBACK Proc(HWND window,UINT message,WPARAM wparam,LPARAM lparam) {
    switch (message) {
    case WM_COMMAND:
        if (LOWORD(wparam)==IDCANCEL) { DebugHub::Hide(); return 0; }
        if (HIWORD(wparam)==BN_CLICKED) {
            bool ok=true;
            if (LOWORD(wparam)==kEnable) ok=WeaponPresets::RequestEnable(SendMessageW(enable_,BM_GETCHECK,0,0)==BST_CHECKED);
            else if (LOWORD(wparam)==kSwap) ok=WeaponPresets::RequestSwap();
            else if (LOWORD(wparam)==kRead) ok=WeaponPresets::RequestReadCurrent();
            else if (LOWORD(wparam)==kSave1||LOWORD(wparam)==kSave2) ok=WeaponPresets::RequestSaveToSet(LOWORD(wparam)==kSave1?0:1);
            else if (LOWORD(wparam)==kUse1||LOWORD(wparam)==kUse2) ok=WeaponPresets::RequestSwitchToSet(LOWORD(wparam)==kUse1?0:1);
            if (!ok) Rejected(); next_=GetTickCount64()+150;
        }
        return 0;
    case WM_SIZE: Layout(); return 0;
    case WM_DPICHANGED_AFTERPARENT: UiLayout::UseFont(window,UiTheme::GetFont(UiTheme::Font::Body,window)); Layout(); return 0;
    case WM_CLOSE: DebugHub::Hide(); return 0;
    case WM_DESTROY: window_=nullptr; return 0;
    default: return DefWindowProcW(window,message,wparam,lparam);
    }
}
}
HWND CreatePage(HWND parent) {
    if (!parent) return nullptr;
    if (!window_) {
        WNDCLASSEXW cls{}; cls.cbSize=sizeof(cls); cls.hInstance=GetModuleHandleW(nullptr); cls.lpfnWndProc=&Proc;
        cls.lpszClassName=kClass; cls.hCursor=LoadCursorW(nullptr,IDC_ARROW); cls.hbrBackground=reinterpret_cast<HBRUSH>(COLOR_BTNFACE+1);
        if (!RegisterClassExW(&cls)&&GetLastError()!=ERROR_CLASS_ALREADY_EXISTS) return nullptr;
        cls.lpszClassName=kLoadoutClass; cls.lpfnWndProc=&LoadoutProc; cls.hbrBackground=nullptr;
        if (!RegisterClassExW(&cls)&&GetLastError()!=ERROR_CLASS_ALREADY_EXISTS) return nullptr;
        window_=CreateWindowExW(0,kClass,L"第二套武器预设",WS_CHILD|WS_CLIPCHILDREN,0,0,800,650,parent,nullptr,cls.hInstance,nullptr);
        if (!window_) return nullptr;
        enable_=Control(L"BUTTON",L"开启第二套武器预设",BS_AUTOCHECKBOX|WS_TABSTOP,kEnable);
        intro_=Control(L"STATIC",L"上方实时读取当前装备；下方两栏是已保存的预设。\r\n在游戏装备菜单配置后，点“保存当前到预设二”；再点“应用预设一”即可切回。左右滑动 PS 触控板也可切换，每次跨预设切换前都会记忆离开的装备。",SS_LEFT);
        swap_=Control(L"BUTTON",L"切换到另一套",BS_PUSHBUTTON|WS_TABSTOP,kSwap);
        read_=Control(L"BUTTON",L"立即读取当前装备",BS_PUSHBUTTON|WS_TABSTOP,kRead);
        live_heading_=Control(L"STATIC",L"当前实际装备（实时）",SS_LEFT);
        live_=Control(kLoadoutClass,L"",0);
        read_status_=Control(L"STATIC",L"等待读取当前装备",SS_LEFT);
        operation_heading_=Control(L"STATIC",L"最近操作结果",SS_LEFT);
        status_=Control(L"STATIC",L"",SS_LEFT); touch_=Control(L"STATIC",L"",SS_LEFT);
        for (int i=0;i<2;++i) {
            heading_[i]=Control(L"STATIC",i==0?L"预设一 / 原始装备":L"预设二",SS_LEFT);
            save_[i]=Control(L"BUTTON",i==0?L"保存当前到预设一":L"保存当前到预设二",BS_PUSHBUTTON|WS_TABSTOP,i==0?kSave1:kSave2);
            use_[i]=Control(L"BUTTON",i==0?L"应用预设一":L"应用预设二",BS_PUSHBUTTON|WS_TABSTOP,i==0?kUse1:kUse2);
            set_[i]=Control(kLoadoutClass,L"",0);
            UiTheme::SetRole(heading_[i],UiTheme::Role::Section);
        }
        UiTheme::SetRole(swap_,UiTheme::Role::Primary);
        UiTheme::SetRole(live_heading_,UiTheme::Role::Section);
        UiTheme::SetRole(operation_heading_,UiTheme::Role::Section);
        DebugHub::Attach(window_,parent,750,600);
    }
    WeaponPresets::RequestReadCurrent(); Refresh(); Layout(); return window_;
}
void Tick() {
    if (window_&&IsWindowVisible(window_)&&GetTickCount64()>=next_) {
        next_=GetTickCount64()+300; WeaponPresets::RequestReadCurrent(); Refresh(); Layout();
    }
}
void Show() { DebugHub::ShowPage(DebugHub::Page::WeaponPresets); }
void Shutdown() { if (window_) DestroyWindow(window_); WeaponIcons::Shutdown(); }
}
