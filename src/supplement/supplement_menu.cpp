#include "supplement_menu.h"
#include "debug_supplement.h"
#include "debug_supplement_catalog.generated.h"
#include "supplement_layout.h"
#include "debug_hub.h"
#include "ui_assets.h"
#include "ui_i18n.h"
#include "ui_layout.h"
#include "ui_motion.h"
#include "ui_theme.h"
#include <objidl.h>
#include <gdiplus.h>
#include <windowsx.h>
#include <algorithm>
#include <cwchar>
#include <cwctype>
#include <iterator>
#include <string>
#include <utility>
#include <vector>

namespace SupplementMenu {
namespace {
using SupplementLayout::Card;
using SupplementLayout::kSections;
constexpr wchar_t kClass[]=L"PBZSupplementPage_v1",kCardClass[]=L"PBZSupplementCard_v1",kChipsClass[]=L"PBZSupplementChips_v1";
constexpr int kSearch=101;
constexpr UINT_PTR kMotion=1;
constexpr ULONGLONG kConfirmMs=3000;
constexpr size_t kItems=std::size(DebugSupplementCatalog::kItems);
constexpr int kSectionCount=static_cast<int>(std::size(kSections));

struct Geometry { int pad=0,title=0,note=0,seg_h=0,gap=0,per_row=1,rows=1,height=0; };
// One card window. The page owns these for the life of its window; GWLP_USERDATA points here.
struct CardView {
    const Card* card=nullptr;
    int section=0,count=0;
    HWND window=nullptr;
    Geometry geo;
    int hot=-1,pressed=-1,focus=0;
    int chosen=-1;                        // option last applied this session (cards that remember)
    int armed=-1; ULONGLONG armed_at=0;   // first click on a confirm card
    uint64_t waiting=0;                   // options requested whose result has not come back
    int flash=-1; bool flash_ok=true;     // the latest result, fading out
    UiMotion::Tween pulse,hover;
    bool keyboard=false;
};
std::vector<CardView> cards_;
std::vector<HWND> headings_,subtitles_;
std::vector<std::pair<int,int>> owner_;  // catalog item -> card, option
HWND window_=nullptr,status_=nullptr,search_=nullptr,hint_=nullptr,chips_=nullptr,empty_=nullptr;
int filter_=-1,chip_hot_=-2;  // -1 is the "All" chip, -2 none
bool chips_keyboard_=false;
std::wstring query_,status_text_,last_;
DebugSupplement::Snapshot snapshot_;
uint64_t seen_generation_=0,seen_result_=0;
UiI18n::Language language_=UiI18n::Language::SimplifiedChinese;
ULONGLONG next_=0;

std::wstring Tr(const wchar_t* text) { return UiI18n::Translate(text?text:L""); }
std::wstring Lower(std::wstring text) { for (wchar_t& c:text) c=static_cast<wchar_t>(std::towlower(c)); return text; }
bool Reduced() { return UiMotion::ReducedMotion(); }
HWND Control(const wchar_t* cls,const wchar_t* text,DWORD style,int id=0,DWORD ex=0) {
    const HWND control=CreateWindowExW(ex,cls,text,WS_CHILD|WS_VISIBLE|style,0,0,10,10,window_,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),GetModuleHandleW(nullptr),nullptr);
    if (control) {
        SendMessageW(control,WM_SETFONT,reinterpret_cast<WPARAM>(UiTheme::GetFont(UiTheme::Font::Body,window_)),FALSE);
        UiI18n::Bind(control,text);
    }
    return control;
}

// ---- catalog state -------------------------------------------------------------------
bool Valid(int item) { return item>=0&&static_cast<size_t>(item)<snapshot_.items.size(); }
bool Removed(int item) { return Valid(item)&&snapshot_.items[static_cast<size_t>(item)].removed; }
bool Usable(int item) { return snapshot_.loaded&&Valid(item)&&snapshot_.items[static_cast<size_t>(item)].available&&!Removed(item); }
const wchar_t* OptionLabel(const CardView& view,int option) { return view.card->options[static_cast<size_t>(option)].label; }
int OptionItem(const CardView& view,int option) { return view.card->options[static_cast<size_t>(option)].item; }

// ---- drawing helpers -------------------------------------------------------------------
Gdiplus::Color Argb(COLORREF color,BYTE alpha=255) { return Gdiplus::Color(alpha,GetRValue(color),GetGValue(color),GetBValue(color)); }
void Rounded(Gdiplus::Graphics& graphics,const RECT& rect,float radius,COLORREF fill,BYTE fill_alpha,COLORREF edge,BYTE edge_alpha,float width=1.f) {
    const float x=static_cast<float>(rect.left)+.5f,y=static_cast<float>(rect.top)+.5f;
    const float w=static_cast<float>(rect.right-rect.left)-1.f,h=static_cast<float>(rect.bottom-rect.top)-1.f;
    const float d=(std::min)(radius*2,(std::min)(w,h));
    if (w<=0||h<=0) return;
    Gdiplus::GraphicsPath path;
    path.AddArc(x,y,d,d,180,90); path.AddArc(x+w-d,y,d,d,270,90); path.AddArc(x+w-d,y+h-d,d,d,0,90); path.AddArc(x,y+h-d,d,d,90,90); path.CloseFigure();
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
// Paints into an off-screen bitmap; off-screen bitmaps start black, so the page colour goes first.
template<class Draw> void Buffered(HWND window,Draw draw) {
    PAINTSTRUCT paint{}; const HDC target=BeginPaint(window,&paint);
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
    EndPaint(window,&paint);
}
COLORREF Paper() { return UiTheme::Mix(UiTheme::Base(),UiTheme::Panel(),72); }

// ---- card geometry -------------------------------------------------------------------
// Title row, an optional note of up to two lines, then the options as equal pills in balanced rows.
Geometry Measure(const CardView& view,int width,HDC dc,HWND reference) {
    const auto m=UiLayout::For(reference);
    Geometry g; g.pad=m.S(14); g.title=m.S(24); g.seg_h=m.S(34); g.gap=m.S(6);
    const int inner=(std::max)(m.S(80),width-2*g.pad);
    const std::wstring note=Tr(view.card->note);
    if (!note.empty()) {
        const HFONT small_font=UiTheme::GetFont(UiTheme::Font::Small,reference);
        g.note=(std::min)(static_cast<int>(Extent(dc,small_font,note,inner).cy),2*static_cast<int>(Extent(dc,small_font,L"Ag").cy));
    }
    const HFONT body=UiTheme::GetFont(UiTheme::Font::Strong,reference);
    int widest=m.S(56);
    for (int i=0;i<view.count;++i) widest=(std::max)(widest,static_cast<int>(Extent(dc,body,Tr(OptionLabel(view,i))).cx)+m.S(26));
    if (view.count>7) widest=(std::min)(widest,(std::max)(m.S(56),(inner-3*g.gap)/4));  // long lists (过场): four a row, ends elided
    g.per_row=std::clamp((inner+g.gap)/(widest+g.gap),1,(std::max)(1,view.count));
    g.rows=(view.count+g.per_row-1)/g.per_row; g.per_row=(view.count+g.rows-1)/g.rows;
    g.height=g.pad+g.title+(g.note?m.S(3)+g.note:0)+m.S(14)+g.rows*g.seg_h+(g.rows-1)*g.gap+g.pad;
    return g;
}
RECT OptionRect(const CardView& view,int option,const RECT& client) {
    const Geometry& g=view.geo;
    const int inner=client.right-2*g.pad,cell=(inner-(g.per_row-1)*g.gap)/g.per_row;
    const int row=option/g.per_row,column=option%g.per_row;
    const int top=g.pad+g.title+(g.note?UiLayout::For(view.window).S(3)+g.note:0)+UiLayout::For(view.window).S(14)+row*(g.seg_h+g.gap);
    const int left=g.pad+column*(cell+g.gap);
    return {left,top,left+cell,top+g.seg_h};
}
int OptionAt(const CardView& view,POINT point,const RECT& client) {
    for (int i=0;i<view.count;++i) { const RECT rect=OptionRect(view,i,client); if (PtInRect(&rect,point)) return i; }
    return -1;
}

// ---- card behaviour -------------------------------------------------------------------
CardView* View(HWND window) { return reinterpret_cast<CardView*>(GetWindowLongPtrW(window,GWLP_USERDATA)); }
void Animate(CardView& view) { if (view.window) { SetTimer(view.window,kMotion,16,nullptr); InvalidateRect(view.window,nullptr,FALSE); } }
bool Armed(const CardView& view,int option,ULONGLONG now) { return view.armed==option&&now-view.armed_at<kConfirmMs; }
void Activate(CardView& view,int option) {
    if (option<0||option>=view.count) return;
    const int item=OptionItem(view,option);
    if (!Usable(item)) {
        DebugHub::Notify(Tr(Removed(item)?L"游戏已移除这一项依赖的功能。":snapshot_.loaded?L"当前游戏缺少这一项需要的函数。":L"补充包仍在加载，请稍候。").c_str());
        return;
    }
    const ULONGLONG now=GetTickCount64();
    if (view.card->confirm&&!Armed(view,option,now)) { view.armed=option; view.armed_at=now; Animate(view); return; }
    view.armed=-1;
    if (!DebugSupplement::RequestCall(item)) { DebugHub::Notify(Tr(L"请求未接受，请稍后重试。").c_str()); Animate(view); return; }
    view.waiting|=uint64_t{1}<<option;
    Animate(view);
}
void PaintCard(HWND window) {
    Buffered(window,[&](HDC dc,Gdiplus::Graphics& graphics,const RECT& client) {
        CardView* view=View(window); if (!view) return;
        const auto m=UiLayout::For(window); const ULONGLONG now=GetTickCount64();
        const bool focused=GetFocus()==window,cues=focused&&view->keyboard;
        const COLORREF paper=Paper(),accent=UiTheme::Accent(),danger=UiTheme::Danger();
        COLORREF edge=UiTheme::Mix(UiTheme::Mix(UiTheme::Border(),UiTheme::Base(),15),accent,static_cast<int>(40*view->hover.At(now)));
        if (cues) edge=accent;
        Rounded(graphics,client,static_cast<float>(m.S(10)),paper,255,edge,255,cues?1.6f:1.f);
        if (view->card->confirm) {
            const RECT stripe{m.S(1),m.S(14),m.S(4),client.bottom-m.S(14)};
            Rounded(graphics,stripe,1.5f,danger,220,danger,0);
        }
        const Geometry& g=view->geo;
        // Badge at the right of the title: pending, the latest result, or that the card asks for a second click.
        std::wstring badge; COLORREF badge_color=UiTheme::MutedText();
        const double pulse=view->pulse.At(now);
        if (view->waiting) badge=Tr(L"执行中…");
        else if (view->flash>=0&&pulse>.02) {
            badge=Tr(view->flash_ok?L"已执行":L"未执行");
            badge_color=UiTheme::Mix(paper,view->flash_ok?accent:danger,static_cast<int>(100*(std::min)(1.0,pulse*1.6)));
        } else if (view->card->confirm) { badge=Tr(L"需确认"); badge_color=UiTheme::Mix(danger,paper,20); }
        const HFONT small_font=UiTheme::GetFont(UiTheme::Font::Small,window),strong=UiTheme::GetFont(UiTheme::Font::Strong,window);
        int title_right=client.right-g.pad;
        if (!badge.empty()) {
            const int badge_width=Extent(dc,small_font,badge).cx;
            Label(dc,badge,{client.right-g.pad-badge_width,g.pad,client.right-g.pad,g.pad+g.title},small_font,badge_color,DT_SINGLELINE|DT_VCENTER|DT_RIGHT);
            title_right-=badge_width+m.S(10);
        }
        Label(dc,Tr(view->card->title),{g.pad,g.pad,title_right,g.pad+g.title},strong,UiTheme::Text(),DT_SINGLELINE|DT_VCENTER|DT_LEFT|DT_END_ELLIPSIS);
        if (g.note) Label(dc,Tr(view->card->note),{g.pad,g.pad+g.title+m.S(3),client.right-g.pad,g.pad+g.title+m.S(3)+g.note},small_font,
            UiTheme::MutedText(),DT_WORDBREAK|DT_EDITCONTROL|DT_END_ELLIPSIS|DT_LEFT);
        const HFONT body=UiTheme::GetFont(UiTheme::Font::Body,window);
        for (int i=0;i<view->count;++i) {
            const RECT rect=OptionRect(*view,i,client);
            const int item=OptionItem(*view,i);
            const bool usable=Usable(item),removed=Removed(item),armed=Armed(*view,i,now),chosen=view->chosen==i;
            const bool hot=view->hot==i&&usable,down=hot&&view->pressed==i,waiting=((view->waiting>>i)&1u)!=0;
            COLORREF fill=UiTheme::Mix(paper,UiTheme::Base(),45),border=UiTheme::Mix(UiTheme::Border(),paper,35),ink=UiTheme::Text();
            if (!usable) { fill=UiTheme::Mix(paper,UiTheme::Base(),18); ink=UiTheme::Mix(UiTheme::MutedText(),paper,30); }
            if (chosen) { fill=UiTheme::Mix(paper,accent,36); border=accent; ink=UiTheme::Mix(UiTheme::Text(),accent,25); }
            if (hot) fill=UiTheme::Mix(fill,UiTheme::Text(),down?16:8);
            if (armed) { fill=UiTheme::Mix(paper,danger,32); border=danger; }
            Rounded(graphics,rect,static_cast<float>(m.S(8)),fill,255,border,255);
            if (view->flash==i&&pulse>.02) Rounded(graphics,rect,static_cast<float>(m.S(8)),view->flash_ok?accent:danger,static_cast<BYTE>(110*pulse),0,0);
            if (cues&&view->focus==i) { RECT ring=rect; InflateRect(&ring,m.S(2),m.S(2)); Rounded(graphics,ring,static_cast<float>(m.S(10)),0,0,accent,255,1.6f); }
            if (armed) {
                const double left=1.0-static_cast<double>(now-view->armed_at)/kConfirmMs;
                const RECT bar{rect.left+m.S(10),rect.bottom-m.S(5),rect.left+m.S(10)+static_cast<int>((rect.right-rect.left-m.S(20))*left),rect.bottom-m.S(3)};
                Rounded(graphics,bar,1.f,danger,255,danger,0);
            }
            std::wstring text=armed?Tr(L"再点确认"):Tr(OptionLabel(*view,i));
            if (waiting) text+=L"…";
            const HFONT font=chosen||armed?strong:body;
            RECT label_rect=rect; InflateRect(&label_rect,-m.S(6),0);
            Label(dc,text,label_rect,font,ink,DT_SINGLELINE|DT_VCENTER|DT_CENTER|DT_END_ELLIPSIS);
            if (removed) {
                const int width=(std::min)(static_cast<int>(Extent(dc,font,text).cx),static_cast<int>(label_rect.right-label_rect.left));
                const int middle=(rect.left+rect.right)/2,y=(rect.top+rect.bottom)/2;
                Gdiplus::Pen pen(Argb(ink),static_cast<float>((std::max)(1,m.S(1))));
                graphics.DrawLine(&pen,middle-width/2-m.S(3),y,middle+width/2+m.S(3),y);
            }
        }
    });
}
CardView* Neighbour(const CardView& view,int step) {
    const auto at=static_cast<int>(&view-cards_.data());
    for (int i=at+step;i>=0&&i<static_cast<int>(cards_.size());i+=step)
        if (cards_[static_cast<size_t>(i)].window&&IsWindowVisible(cards_[static_cast<size_t>(i)].window)) return &cards_[static_cast<size_t>(i)];
    return nullptr;
}
LRESULT CALLBACK CardProc(HWND window,UINT message,WPARAM wparam,LPARAM lparam) {
    if (message==WM_NCCREATE) {
        auto* view=static_cast<CardView*>(reinterpret_cast<CREATESTRUCTW*>(lparam)->lpCreateParams);
        SetWindowLongPtrW(window,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(view)); if (view) view->window=window;
        return DefWindowProcW(window,message,wparam,lparam);
    }
    CardView* view=View(window);
    if (!view) return DefWindowProcW(window,message,wparam,lparam);
    const ULONGLONG now=GetTickCount64();
    RECT client{}; GetClientRect(window,&client);
    switch (message) {
    case WM_NCDESTROY: view->window=nullptr; SetWindowLongPtrW(window,GWLP_USERDATA,0); break;
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: PaintCard(window); return 0;
    case WM_SIZE: InvalidateRect(window,nullptr,FALSE); return 0;
    case WM_TIMER: {
        if (wparam!=kMotion) break;
        if (view->armed>=0&&!Armed(*view,view->armed,now)) view->armed=-1;
        if (!view->hover.Running(now)&&!view->pulse.Running(now)&&view->armed<0) { KillTimer(window,kMotion); if (view->pulse.At(now)<=.02) view->flash=-1; }
        InvalidateRect(window,nullptr,FALSE); return 0;
    }
    case WM_MOUSEMOVE: {
        const int hit=OptionAt(*view,{GET_X_LPARAM(lparam),GET_Y_LPARAM(lparam)},client);
        if (hit!=view->hot) { view->hot=hit; InvalidateRect(window,nullptr,FALSE); }
        if (view->hover.to<1) { view->hover.Set(1,140,now,Reduced()); Animate(*view); }
        TRACKMOUSEEVENT track{sizeof(track),TME_LEAVE,window,0}; TrackMouseEvent(&track);
        return 0;
    }
    case WM_MOUSELEAVE: view->hot=-1; view->hover.Set(0,260,now,Reduced()); Animate(*view); return 0;
    case WM_SETCURSOR:
        if (LOWORD(lparam)==HTCLIENT&&view->hot>=0) { SetCursor(LoadCursorW(nullptr,IDC_HAND)); return TRUE; }
        break;
    case WM_LBUTTONDOWN: {
        SetFocus(window); view->keyboard=false;
        view->pressed=OptionAt(*view,{GET_X_LPARAM(lparam),GET_Y_LPARAM(lparam)},client);
        if (view->pressed>=0) { view->focus=view->pressed; SetCapture(window); }
        InvalidateRect(window,nullptr,FALSE); return 0;
    }
    case WM_LBUTTONUP: {
        // ReleaseCapture sends WM_CAPTURECHANGED, which clears the press: read it first.
        const int pressed=view->pressed; view->pressed=-1;
        if (GetCapture()==window) ReleaseCapture();
        const int hit=OptionAt(*view,{GET_X_LPARAM(lparam),GET_Y_LPARAM(lparam)},client);
        if (pressed>=0&&hit==pressed) Activate(*view,pressed);
        InvalidateRect(window,nullptr,FALSE); return 0;
    }
    case WM_CAPTURECHANGED: view->pressed=-1; InvalidateRect(window,nullptr,FALSE); return 0;
    case WM_GETDLGCODE: {
        const MSG* msg=reinterpret_cast<const MSG*>(lparam);
        LRESULT code=DLGC_WANTARROWS|DLGC_WANTCHARS;
        if (msg&&msg->message==WM_KEYDOWN&&(msg->wParam==VK_RETURN||msg->wParam==VK_SPACE)) code|=DLGC_WANTMESSAGE;
        return code;
    }
    case WM_KEYDOWN: {
        view->keyboard=true;
        if (wparam==VK_LEFT) view->focus=(std::max)(0,view->focus-1);
        else if (wparam==VK_RIGHT) view->focus=(std::min)(view->count-1,view->focus+1);
        else if (wparam==VK_UP||wparam==VK_DOWN) {
            if (CardView* next=Neighbour(*view,wparam==VK_UP?-1:1)) {
                next->keyboard=true; next->focus=(std::min)(view->focus,next->count-1); SetFocus(next->window);
            }
        } else if (wparam==VK_RETURN||wparam==VK_SPACE) Activate(*view,view->focus);
        else break;
        InvalidateRect(window,nullptr,FALSE); return 0;
    }
    case WM_CHAR: if (wparam==VK_RETURN||wparam==L' ') return 0; break;
    case WM_SETFOCUS: case WM_KILLFOCUS: InvalidateRect(window,nullptr,FALSE); return 0;
    default: break;
    }
    return DefWindowProcW(window,message,wparam,lparam);
}

// ---- section filter -------------------------------------------------------------------
// "All" followed by every section, as a wrapping row of pills; the selected one wears the game's brush stroke.
struct Chip { RECT rect{}; int filter=-1; };
std::wstring ChipText(int filter) { return Tr(filter<0?L"全部":kSections[filter].title); }
std::vector<Chip> Chips(HWND window,int width) {
    std::vector<Chip> chips; if (!window) return chips;
    const auto m=UiLayout::For(window); const HDC dc=GetDC(window);
    const HFONT font=UiTheme::GetFont(UiTheme::Font::Strong,window);
    int x=0,y=0; const int height=m.S(34),gap=m.S(6);
    for (int filter=-1;filter<kSectionCount;++filter) {
        const int w=Extent(dc,font,ChipText(filter)).cx+m.S(34);
        if (x>0&&x+w>width) { x=0; y+=height+gap; }
        chips.push_back({{x,y,x+w,y+height},filter}); x+=w+gap;
    }
    ReleaseDC(window,dc);
    return chips;
}
int ChipsHeight(int width) { const auto chips=Chips(chips_,width); return chips.empty()?0:chips.back().rect.bottom; }
void Layout();
void SetFilter(int filter) {
    if (filter==filter_) return;
    filter_=filter; InvalidateRect(chips_,nullptr,FALSE); Layout();
}
void PaintChips(HWND window) {
    Buffered(window,[&](HDC dc,Gdiplus::Graphics& graphics,const RECT& client) {
        const auto m=UiLayout::For(window);
        const bool art=!UiTheme::HighContrast(),cues=GetFocus()==window&&chips_keyboard_;
        const HFONT font=UiTheme::GetFont(UiTheme::Font::Strong,window);
        for (const Chip& chip:Chips(window,client.right)) {
            const bool selected=chip.filter==filter_,hot=chip.filter==chip_hot_;
            const Gdiplus::RectF area(static_cast<float>(chip.rect.left),static_cast<float>(chip.rect.top+m.S(2)),
                static_cast<float>(chip.rect.right-chip.rect.left),static_cast<float>(chip.rect.bottom-chip.rect.top-m.S(4)));
            COLORREF ink=selected?UiTheme::Text():hot?UiTheme::Text():UiTheme::MutedText();
            if (art&&(selected||hot)) {
                UiAssets::DrawBar(graphics,UiAssets::Id::SelectedBar,area,selected?1.f:.16f);
                if (selected) ink=RGB(0x14,0x12,0x10);
            } else if (selected||hot) Rounded(graphics,chip.rect,static_cast<float>(m.S(9)),selected?UiTheme::Selection():UiTheme::Panel(),255,UiTheme::Border(),255);
            if (cues&&selected) { RECT ring=chip.rect; InflateRect(&ring,m.S(1),0); Rounded(graphics,ring,static_cast<float>(m.S(9)),0,0,UiTheme::Accent(),255,1.6f); }
            Label(dc,ChipText(chip.filter),chip.rect,font,ink,DT_SINGLELINE|DT_VCENTER|DT_CENTER);
        }
    });
}
int ChipAt(HWND window,LPARAM lparam) {
    RECT client{}; GetClientRect(window,&client);
    const POINT point{GET_X_LPARAM(lparam),GET_Y_LPARAM(lparam)};
    for (const Chip& chip:Chips(window,client.right)) if (PtInRect(&chip.rect,point)) return chip.filter;
    return -2;
}
LRESULT CALLBACK ChipsProc(HWND window,UINT message,WPARAM wparam,LPARAM lparam) {
    switch (message) {
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: PaintChips(window); return 0;
    case WM_SIZE: InvalidateRect(window,nullptr,FALSE); return 0;
    case WM_MOUSEMOVE: {
        const int hit=ChipAt(window,lparam);
        if (hit!=chip_hot_) { chip_hot_=hit; InvalidateRect(window,nullptr,FALSE); }
        TRACKMOUSEEVENT track{sizeof(track),TME_LEAVE,window,0}; TrackMouseEvent(&track); return 0;
    }
    case WM_MOUSELEAVE: chip_hot_=-2; InvalidateRect(window,nullptr,FALSE); return 0;
    case WM_SETCURSOR:
        if (LOWORD(lparam)==HTCLIENT&&chip_hot_>=-1) { SetCursor(LoadCursorW(nullptr,IDC_HAND)); return TRUE; }
        break;
    case WM_LBUTTONDOWN: { SetFocus(window); chips_keyboard_=false; const int hit=ChipAt(window,lparam); if (hit>=-1) SetFilter(hit); InvalidateRect(window,nullptr,FALSE); return 0; }
    case WM_GETDLGCODE: return DLGC_WANTARROWS;
    case WM_KEYDOWN:
        if (wparam==VK_LEFT||wparam==VK_RIGHT) {
            chips_keyboard_=true;
            SetFilter(std::clamp(filter_+(wparam==VK_LEFT?-1:1),-1,kSectionCount-1));
            InvalidateRect(window,nullptr,FALSE); return 0;
        }
        break;
    case WM_SETFOCUS: case WM_KILLFOCUS: InvalidateRect(window,nullptr,FALSE); return 0;
    default: break;
    }
    return DefWindowProcW(window,message,wparam,lparam);
}

// ---- page -------------------------------------------------------------------
bool Matches(const CardView& view) {
    if (filter_>=0&&view.section!=filter_) return false;
    if (query_.empty()) return true;
    auto has=[](const wchar_t* text) {
        return text&&*text&&(Lower(text).find(query_)!=std::wstring::npos||Lower(Tr(text)).find(query_)!=std::wstring::npos);
    };
    if (has(view.card->title)||has(view.card->note)||has(kSections[view.section].title)) return true;
    for (int i=0;i<view.count;++i)
        if (has(OptionLabel(view,i))||has(DebugSupplementCatalog::kItems[static_cast<size_t>(OptionItem(view,i))].label)) return true;
    return false;
}
void Show(HWND control,bool show) { if (control&&(IsWindowVisible(control)!=FALSE)!=show) ShowWindow(control,show?SW_SHOWNA:SW_HIDE); }
void Layout() {
    if (!window_||!status_) return;
    RECT rect{}; GetClientRect(window_,&rect); const auto m=UiLayout::For(window_);
    const int x=m.Margin(),gap=m.Gap(),w=(std::max)(m.S(420),static_cast<int>(rect.right)-2*x);
    int y=x;
    const int status_height=UiLayout::TextHeight(status_,w-m.S(28))+m.S(20);
    UiLayout::Place(status_,x,y,w,status_height); y+=status_height+gap;
    UiLayout::Place(search_,x,y,(std::min)(w,m.S(360)),m.Row()); y+=m.Row()+gap/2;
    const int hint_height=UiLayout::TextHeight(hint_,w);
    UiLayout::Place(hint_,x,y,w,hint_height); y+=hint_height+gap;
    const int chips_height=ChipsHeight(w);
    UiLayout::Place(chips_,x,y,w,chips_height); y+=chips_height+gap;
    const int columns=(std::max)(1,(w+gap)/(m.S(300)+gap)),card_width=(w-gap*(columns-1))/columns;
    const HDC dc=GetDC(window_);
    bool any=false;
    for (int s=0;s<kSectionCount;++s) {
        std::vector<CardView*> shown;
        for (CardView& view:cards_) if (view.section==s) {
            if (Matches(view)) shown.push_back(&view); else Show(view.window,false);
        }
        const bool visible=!shown.empty(),subtitle=visible&&*kSections[s].subtitle;
        Show(headings_[static_cast<size_t>(s)],visible); Show(subtitles_[static_cast<size_t>(s)],subtitle);
        if (!visible) continue;
        any=true; y+=gap/2;
        UiLayout::Place(headings_[static_cast<size_t>(s)],x,y,w,m.Section()); y+=m.Section()+gap/2;
        if (subtitle) {
            const int h=UiLayout::TextHeight(subtitles_[static_cast<size_t>(s)],w);
            UiLayout::Place(subtitles_[static_cast<size_t>(s)],x,y,w,h); y+=h+gap;
        }
        for (size_t first=0;first<shown.size();first+=static_cast<size_t>(columns)) {
            const size_t last=(std::min)(shown.size(),first+static_cast<size_t>(columns));
            int row_height=0;
            for (size_t i=first;i<last;++i) { shown[i]->geo=Measure(*shown[i],card_width,dc,window_); row_height=(std::max)(row_height,shown[i]->geo.height); }
            for (size_t i=first;i<last;++i) {
                UiLayout::Place(shown[i]->window,x+static_cast<int>(i-first)*(card_width+gap),y,card_width,row_height);
                Show(shown[i]->window,true); InvalidateRect(shown[i]->window,nullptr,FALSE);
            }
            y+=row_height+gap;
        }
    }
    ReleaseDC(window_,dc);
    Show(empty_,!any);
    if (!any) { const int h=UiLayout::TextHeight(empty_,w); UiLayout::Place(empty_,x,y,w,h); y+=h+gap; }
    DebugHub::SetMinimum(window_,m.S(420)+2*x,y+x);
    // Themed statics draw without erasing: moved ones would leave their old text behind.
    RedrawWindow(window_,nullptr,nullptr,RDW_INVALIDATE|RDW_ERASE|RDW_ALLCHILDREN);
}
void UpdateStatus() {
    int available=0;
    for (const auto& item:snapshot_.items) available+=item.available&&!item.removed?1:0;
    std::wstring text;
    if (!snapshot_.mounted) text=Tr(L"补充包没有挂载，不能使用这些功能。");
    else if (!snapshot_.loaded) text=Tr(L"正在读取补充包…");
    else {
        wchar_t line[256]{};
        _snwprintf_s(line,_countof(line),_TRUNCATE,Tr(L"补充包已就绪：%d / %zu 项可用，本次已执行 %u 次").c_str(),available,kItems,snapshot_.calls);
        text=line;
    }
    // Second line: the latest result here, else what the backend reported while the Blueprint is not loaded.
    if (!last_.empty()) text+=L"\r\n"+last_;
    else if (!snapshot_.loaded&&!snapshot_.status.empty()) text+=L"\r\n"+Tr(snapshot_.status.c_str());
    if (text==status_text_) return;
    const bool lines=std::count(text.begin(),text.end(),L'\n')!=std::count(status_text_.begin(),status_text_.end(),L'\n');
    status_text_=text; UiI18n::SetText(status_,text.c_str());
    if (lines) Layout();
}
void ApplyResult(const DebugSupplement::Result& result) {
    if (result.item<0||static_cast<size_t>(result.item)>=owner_.size()) return;
    const auto [card,option]=owner_[static_cast<size_t>(result.item)];
    if (card<0) return;
    CardView& view=cards_[static_cast<size_t>(card)];
    view.waiting&=~(uint64_t{1}<<option);
    view.flash=option; view.flash_ok=result.ok;
    view.pulse.Jump(1); view.pulse.Set(0,1400,GetTickCount64(),Reduced());
    if (result.ok&&view.card->remembers) view.chosen=option;
    last_=Tr(result.ok?L"已执行：":L"未执行：")+Tr(view.card->title)+L" · "+Tr(OptionLabel(view,option));
    if (!result.ok) DebugHub::Notify(Tr(snapshot_.status.c_str()).c_str());
    Animate(view);
}
void Refresh() {
    DebugSupplement::Snapshot snapshot=DebugSupplement::GetSnapshot();
    if (snapshot.generation==seen_generation_&&!snapshot_.items.empty()) return;
    snapshot_=std::move(snapshot); seen_generation_=snapshot_.generation;
    for (const auto& result:snapshot_.results) if (result.sequence>seen_result_) { seen_result_=result.sequence; ApplyResult(result); }
    UpdateStatus();
    for (const CardView& view:cards_) if (view.window) InvalidateRect(view.window,nullptr,FALSE);
}
LRESULT CALLBACK Proc(HWND window,UINT message,WPARAM wparam,LPARAM lparam) {
    switch (message) {
    case WM_COMMAND:
        if (LOWORD(wparam)==IDCANCEL) { DebugHub::Hide(); return 0; }
        if (LOWORD(wparam)==kSearch&&HIWORD(wparam)==EN_CHANGE) {
            std::wstring text=UiLayout::Caption(search_);
            const size_t first=text.find_first_not_of(L" \t"),last=text.find_last_not_of(L" \t");
            query_=first==std::wstring::npos?std::wstring():Lower(text.substr(first,last-first+1));
            Layout();
        }
        return 0;
    case WM_SIZE: Layout(); return 0;
    case WM_DPICHANGED_AFTERPARENT:
        UiLayout::UseFont(window,UiTheme::GetFont(UiTheme::Font::Body,window)); Layout();
        InvalidateRect(chips_,nullptr,FALSE); return 0;
    case WM_CLOSE: DebugHub::Hide(); return 0;
    case WM_DESTROY: window_=nullptr; return 0;
    default: return DefWindowProcW(window,message,wparam,lparam);
    }
}
bool RegisterClasses() {
    WNDCLASSEXW cls{}; cls.cbSize=sizeof(cls); cls.hInstance=GetModuleHandleW(nullptr); cls.hCursor=LoadCursorW(nullptr,IDC_ARROW);
    struct { const wchar_t* name; WNDPROC proc; HBRUSH brush; } classes[]={
        {kClass,&Proc,reinterpret_cast<HBRUSH>(COLOR_BTNFACE+1)},{kCardClass,&CardProc,nullptr},{kChipsClass,&ChipsProc,nullptr}};
    for (const auto& c:classes) {
        cls.lpszClassName=c.name; cls.lpfnWndProc=c.proc; cls.hbrBackground=c.brush;
        if (!RegisterClassExW(&cls)&&GetLastError()!=ERROR_CLASS_ALREADY_EXISTS) return false;
    }
    return true;
}
}

HWND CreatePage(HWND parent) {
    if (!parent) return nullptr;
    if (!window_) {
        if (!RegisterClasses()) return nullptr;
        window_=CreateWindowExW(0,kClass,L"调试补充",WS_CHILD|WS_CLIPCHILDREN,0,0,800,650,parent,nullptr,GetModuleHandleW(nullptr),nullptr);
        if (!window_) return nullptr;
        status_=Control(L"STATIC",L"正在读取补充包…",SS_LEFT);
        search_=Control(L"EDIT",L"",WS_TABSTOP|ES_AUTOHSCROLL,kSearch,WS_EX_CLIENTEDGE);
        UiI18n::Cue(search_,L"搜索功能或选项");
        hint_=Control(L"STATIC",L"点选项立即生效。左侧有红条的卡片会改写存档或进度：点一次后，三秒内再点一次才执行。带删除线的选项已被游戏移除。",SS_LEFT);
        chips_=CreateWindowExW(0,kChipsClass,L"",WS_CHILD|WS_VISIBLE|WS_TABSTOP,0,0,10,10,window_,nullptr,GetModuleHandleW(nullptr),nullptr);
        empty_=Control(L"STATIC",L"没有匹配的功能。",SS_LEFT);
        UiTheme::SetRole(status_,UiTheme::Role::Card);
        UiTheme::SetRole(hint_,UiTheme::Role::Hint);
        UiTheme::SetRole(empty_,UiTheme::Role::Hint);
        size_t total=0;
        for (const auto& section:kSections) total+=section.count;
        cards_.clear(); cards_.reserve(total);
        owner_.assign(kItems,{-1,-1});
        for (int s=0;s<kSectionCount;++s) {
            headings_.push_back(Control(L"STATIC",kSections[s].title,SS_LEFT));
            UiTheme::SetRole(headings_.back(),UiTheme::Role::Section);
            subtitles_.push_back(Control(L"STATIC",kSections[s].subtitle,SS_LEFT));
            UiTheme::SetRole(subtitles_.back(),UiTheme::Role::Hint);
            for (size_t c=0;c<kSections[s].count;++c) {
                CardView view; view.card=&kSections[s].cards[c]; view.section=s;
                while (view.count<static_cast<int>(view.card->options.size())&&view.card->options[static_cast<size_t>(view.count)].label) ++view.count;
                cards_.push_back(view);
                for (int o=0;o<view.count;++o) {
                    const int item=OptionItem(view,o);
                    if (item>=0&&static_cast<size_t>(item)<kItems) owner_[static_cast<size_t>(item)]={static_cast<int>(cards_.size()-1),o};
                }
            }
        }
        for (CardView& view:cards_)
            CreateWindowExW(0,kCardClass,view.card->title,WS_CHILD|WS_TABSTOP,0,0,10,10,window_,nullptr,GetModuleHandleW(nullptr),&view);
        // Results finished before the page existed (console commands) are not replayed as fresh feedback.
        snapshot_=DebugSupplement::GetSnapshot(); seen_generation_=snapshot_.generation;
        if (!snapshot_.results.empty()) seen_result_=snapshot_.results.back().sequence;
        language_=UiI18n::GetLanguage();
        UpdateStatus();
        DebugHub::Attach(window_,parent,460,600);
    }
    DebugSupplement::RequestLoad();
    Refresh(); Layout(); return window_;
}
void Tick() {
    if (!window_||!IsWindowVisible(window_)) return;
    const ULONGLONG now=GetTickCount64();
    if (now<next_) return;
    next_=now+120;
    if (UiI18n::GetLanguage()!=language_) {
        language_=UiI18n::GetLanguage(); status_text_.clear(); last_.clear();
        UpdateStatus(); Layout(); InvalidateRect(chips_,nullptr,FALSE);
    }
    Refresh();
}
void Shutdown() {
    if (window_) DestroyWindow(window_);
    window_=nullptr; cards_.clear(); headings_.clear(); subtitles_.clear(); owner_.clear();
}
}
