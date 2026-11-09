// Hidden/offline only: actual page geometry and fonts, no input injection, focus or capture.
#include "../src/supplement/supplement_menu.cpp"
#include "sequence_request.h"
#include "ui_host.h"
#include <cstdio>
#include <sstream>

namespace { int hidden=0,requests=0; bool accept=true; }
namespace DebugHub {
void Hide() { ++hidden; }
bool Notify(const wchar_t*) { return true; }
void Attach(HWND,HWND,int,int) {}
void SetMinimum(HWND,int,int) {}
}
namespace UiHost { bool Attach(HWND) { return true; } POINT ClientOrigin() { return {}; } }
namespace DebugSupplement {
Snapshot GetSnapshot() { return {}; }
bool RequestLoad() { return true; }
bool RequestCall(int32_t) { ++requests; return accept; }
}
int main() {
    using namespace SupplementMenu;
    using SupplementSequence::Request;
    using Step=Request::Step;
    SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_UNAWARE);
    const HWND foreground=GetForegroundWindow(); RECT clip{}; GetClipCursor(&clip);
    int checks=0,failures=0;
    auto check=[&](bool value,const char* message) { ++checks; if (!value) { ++failures; std::printf("FAIL %s\n",message); } };
    for (int run=0;run<100;++run) {
        Request r;
        check(r.Begin(202,100),"accept first request");
        check(!r.Begin(203,101)&&r.item==202,"reject overlapping request without replacing key");
        check(r.Poll(102,true,true,false)==Step::Wait,"paused world cannot invoke latent event");
        check(r.Poll(103,false,false,false)==Step::Wait,"missing world cannot invoke");
        check(r.Poll(104,true,false,false)==Step::Invoke,"unpaused world dispatches");
        for (int tick=0;tick<120;++tick) check(r.Poll(105+tick,true,false,false)==Step::Wait,"dispatch once, no premature success");
        check(r.Poll(1000,true,false,true)==Step::Started,"only observed playback succeeds");
        r.Clear(); check(r.item==-1&&r.Begin(203,2000),"reuse after completion");
        check(r.Poll(47000,false,true,false)==Step::Timeout,"bounded failure during travel or external pause");
    }
    for (const wchar_t* bad:{L"",L"a",L"a#0",L"#0#0",L"a##0",L"a#0#",L"a#-1#0",L"a#1#0x",L"a#2147483648#0",L"a#0#1#2"}) {
        SupplementSequence::Key k; check(!SupplementSequence::Parse(bad,k),"reject malformed play key");
    }
    UiTheme::Initialize();
    WNDCLASSW cls{}; cls.lpfnWndProc=DefWindowProcW; cls.hInstance=GetModuleHandleW(nullptr); cls.lpszClassName=L"SupplementHiddenTest"; RegisterClassW(&cls);
    HWND host=CreateWindowExW(WS_EX_NOACTIVATE|WS_EX_TOOLWINDOW,cls.lpszClassName,L"",WS_POPUP,0,0,1440,900,nullptr,nullptr,cls.hInstance,nullptr);
    check(host!=nullptr,"hidden host");
    HDC dc=GetDC(host);
    for (int language=0;language<2;++language) {
        UiI18n::SetLanguage(language?UiI18n::Language::English:UiI18n::Language::SimplifiedChinese);
        for (int width:{420,720,980,1440}) for (const auto& card:SupplementLayout::kPlaySeq) {
            CardView view; view.card=&card; view.window=host;
            while (view.count<int(card.options.size())&&card.options[view.count].label) ++view.count;
            view.geo=Measure(view,width,dc,host);
            const auto font=UiTheme::GetFont(UiTheme::Font::Strong,host);
            RECT client{0,0,width,view.geo.height};
            for (int i=0;i<view.count;++i) {
                const auto& item=DebugSupplementCatalog::kItems[OptionItem(view,i)];
                check(std::wstring(OptionLabel(view,i))==item.label,"complete sequence and flow identifier retained");
                SupplementSequence::Key key; check(SupplementSequence::Parse(item.arg,key),"every catalog play key valid");
                const RECT box=OptionRect(view,i,client);
                check(box.left>=0&&box.right<=width&&box.bottom<=client.bottom,"option stays inside map card");
                const auto original=SequenceText(view,i);
                const auto wrapped=WrappedSequence(dc,font,original,box.right-box.left-24);
                auto flat=[](std::wstring text) { text.erase(std::remove(text.begin(),text.end(),L'\n'),text.end()); return text; };
                check(flat(wrapped)==flat(original),"wrapping never discards name characters");
                std::wistringstream lines(wrapped); std::wstring line; int count=0;
                while (std::getline(lines,line)) { ++count; check(Extent(dc,font,line).cx<=box.right-box.left-24,"each name line fits without ellipsis"); }
                check(count*Extent(dc,font,L"Ag").cy<=box.bottom-box.top-20,"all lines fit vertically");
            }
        }
    }
    snapshot_.loaded=true; snapshot_.items.resize(kItems); for (auto& item:snapshot_.items) item.available=true;
    // Render one real card to a bitmap through WM_PRINTCLIENT, with a hidden, non-activating parent.
    RegisterClasses();
    CardView render; render.card=&SupplementLayout::kPlaySeq[0]; render.count=12;
    UiI18n::SetLanguage(UiI18n::Language::SimplifiedChinese);
    render.geo=Measure(render,980,dc,host);
    HWND card=CreateWindowExW(0,kCardClass,L"",WS_CHILD,0,0,980,render.geo.height,host,nullptr,cls.hInstance,&render);
    HDC memory=CreateCompatibleDC(dc); HBITMAP bitmap=CreateCompatibleBitmap(dc,980,render.geo.height); auto old=SelectObject(memory,bitmap);
    SendMessageW(card,WM_PRINTCLIENT,reinterpret_cast<WPARAM>(memory),PRF_CLIENT);
    CLSID png{}; CLSIDFromString(L"{557cf406-1a04-11d3-9a73-0000f81ef32e}",&png);
    { Gdiplus::Bitmap output(bitmap,nullptr); check(output.Save(L"local/validation/supplement_sequence_20261109/sequence_card.png",&png)==Gdiplus::Ok,"hidden card rendered"); }
    SelectObject(memory,old); DeleteObject(bitmap); DeleteDC(memory); DestroyWindow(card);
    // Direct activation has no OS input side effects: mocks verify confirmation and hide only after acceptance.
    CardView view; view.card=&SupplementLayout::kPlaySeq[0]; view.count=12;
    Activate(view,0); check(hidden==0&&requests==0,"first click only arms");
    Activate(view,0); check(hidden==1&&requests==1,"confirmed sequence queues and closes panel");
    accept=false; Activate(view,1); Activate(view,1); check(hidden==1&&requests==2,"rejected request leaves panel visible");
    ReleaseDC(host,dc); DestroyWindow(host);
    RECT after{}; GetClipCursor(&after);
    check(GetForegroundWindow()==foreground&&!GetCapture()&&EqualRect(&clip,&after),"no focus capture or clip change");
    std::printf("checks=%d failures=%d\n",checks,failures); return failures?1:0;
}
