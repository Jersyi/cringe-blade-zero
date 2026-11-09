// Reuse only the offline game stubs. The interactive preview entry point is never called.
#define wmain UnusedInteractivePreviewEntry
#include "combo_menu_preview.cpp"
#undef wmain
#include "combo_file.h"
#include "settings_store.h"
#include "weapon_icons.h"
#include <filesystem>

int wmain(int argc,wchar_t** argv) {
    namespace fs=std::filesystem;
    using namespace ComboMenu::Probe;
    SetErrorMode(SEM_FAILCRITICALERRORS|SEM_NOGPFAULTERRORBOX);
    SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_UNAWARE);
    const HWND foreground=GetForegroundWindow();
    RECT clip_before{}; GetClipCursor(&clip_before);
    int failures=0,checks=0;
    const auto check=[&](bool ok,const char* what) { ++checks; if (!ok) { ++failures; std::fprintf(stderr,"FAIL %s\n",what); } };
    const auto contains=[](const RECT& outer,const RECT& inner) { return inner.right>inner.left&&inner.bottom>inner.top&&inner.left>=outer.left&&inner.right<=outer.right&&inner.top>=outer.top&&inner.bottom<=outer.bottom; };
    const fs::path root=fs::temp_directory_path()/(L"pbz_combo_library_"+std::to_wstring(GetCurrentProcessId())+L"_"+std::to_wstring(GetTickCount64()));
    if (!fs::create_directory(root)) return 2;
    SettingsStore::Initialize((root/L"test.ini").c_str());
    SettingsStore::Put(L"NpcCombo",L"File",L"layout_test");
    const auto packed=ComboFile::Pack({});
    FILE* seed=nullptr; _wfopen_s(&seed,(root/L"layout_test.combo").c_str(),L"wb");
    if (!seed) return 2;
    fwrite(packed.data(),1,packed.size(),seed); fclose(seed);
    NpcCombo::SetFolder(root.wstring()); UiCursor::SetHandsOff(true); UiTheme::Initialize();
    NpcRetarget::SkinMoves moves; NpcRetarget::GetSkinMoves(0,moves);
    auto config=NpcCombo::Decode(L"E1"); config.placed=true;
    for (int i=0;i<8;++i) { NpcCombo::Node node; node.ability=moves.skills[static_cast<size_t>(i)%moves.skills.size()]; node.x=200+(i%3)*300; node.y=80+(i/3)*170; config.chain.push_back(node); }
    NpcCombo::Node boss; boss.type=NpcCombo::NodeType::BossSkill; boss.boss=moves.profile; boss.ability=moves.skills.front(); boss.x=1100; boss.y=900;
    const int boss_index=static_cast<int>(config.chain.size()); config.chain.push_back(boss);
    NpcCombo::Node trim; trim.type=NpcCombo::NodeType::Trim; trim.trim_from=500; trim.trim_to=2500; trim.x=1100; trim.y=1200;
    const int trim_index=static_cast<int>(config.chain.size()); config.chain.push_back(trim);
    check(NpcCombo::LinkModifier(config,boss_index,trim_index),"trim fixture linked to a real boss ability");
    NpcCombo::RecordClip(boss.ability,4.0f);
    NpcCombo::Save(moves.profile,config);
    WNDCLASSW cls{}; cls.lpfnWndProc=DefWindowProcW; cls.hInstance=GetModuleHandleW(nullptr); cls.lpszClassName=L"ComboLibraryHiddenHost"; RegisterClassW(&cls);
    // No WS_VISIBLE and no message loop/input messages. A hidden parent keeps its child hidden too.
    host_=CreateWindowExW(WS_EX_NOACTIVATE|WS_EX_TOOLWINDOW,cls.lpszClassName,L"",WS_POPUP,0,0,980,720,nullptr,nullptr,cls.hInstance,nullptr);
    page_=ComboMenu::CreatePage(host_); check(page_!=nullptr,"hidden page created");
    const auto generation=NpcCombo::Generation();
    for (int language=0;language<2;++language) {
        UiI18n::SetLanguage(language?UiI18n::Language::English:UiI18n::Language::SimplifiedChinese);
        for (int width:{720,980,1440}) {
            SetWindowPos(host_,nullptr,0,0,width,720,SWP_NOMOVE|SWP_NOZORDER|SWP_NOACTIVATE);
            SetWindowPos(page_,nullptr,0,0,width,720,SWP_NOMOVE|SWP_NOZORDER|SWP_NOACTIVATE);
            std::set<int> visible_tools;
            for (int category=0;category<5;++category) {
                Library(0,category); LibraryTurn(-1000);
                const RECT rail=LibraryBox(),canvas=Canvas();
                check(rail.left>canvas.right&&rail.right<=width,"right library stays outside canvas and inside page");
                check(contains(rail,LibraryTabBox(0))&&contains(rail,LibraryTabBox(1)),"exactly two primary tabs fit");
                for (int c=0;c<5;++c) check(contains(rail,LibraryCategoryBox(c)),"node category stays inside node tab");
                const int pages=LibraryPageCount();
                for (int page=0;page<pages;++page) {
                    check(LibraryPage()==page,"page advances once");
                    for (int button=0;button<37;++button) {
                        const int owner=ToolCategory(button); if (owner<0) continue;
                        const RECT box=ToolBox(button);
                        if (box.right>box.left) { check(owner==category&&contains(rail,box),"visible node belongs to selected category and fits"); visible_tools.insert(button); }
                        else if (owner!=category) check(box.right==box.left,"other categories cannot paint or receive a click");
                    }
                    LibraryTurn(1);
                }
                check(LibraryPage()==pages-1,"last page cannot overshoot"); LibraryTurn(-1000); check(LibraryPage()==0,"first page cannot undershoot");
            }
            check(visible_tools.size()==27,"all 27 creation tools remain reachable");
            Library(1,0); LibraryTurn(-1000);
            for (int c=0;c<5;++c) check(LibraryCategoryBox(c).right==LibraryCategoryBox(c).left,"weapon tab hides node categories");
            check(WeaponRows()>0&&LibraryPageCount()>1,"weapon list is paginated");
            LibraryTurn(1000); check(LibraryPage()==LibraryPageCount()-1,"weapon page clamps to final page");
            check(!IsWindowVisible(host_)&&!IsWindowVisible(page_)&&GetCapture()!=page_,"render host remains hidden without mouse capture");
            if (argc>1&&width==980) {
                for (int view=0;view<3;++view) {
                    Library(view==2?1:0,view==1?4:2); LibraryTurn(-1000);
                    RECT area{}; GetClientRect(page_,&area);
                    HDC screen=GetDC(page_),dc=CreateCompatibleDC(screen);
                    HBITMAP bitmap=CreateCompatibleBitmap(screen,area.right,area.bottom); const HGDIOBJ old=SelectObject(dc,bitmap);
                    SendMessageW(page_,WM_PRINTCLIENT,reinterpret_cast<WPARAM>(dc),PRF_CLIENT);
                    const fs::path path=fs::path(argv[1])/(L"library_"+std::to_wstring(language)+L"_"+std::to_wstring(view)+L".png");
                    { Gdiplus::Bitmap image(bitmap,nullptr); CLSID png{}; check(CLSIDFor(L"image/png",&png)&&image.Save(path.c_str(),&png,nullptr)==Gdiplus::Ok,"hidden page renders without showing a window"); }
                    SelectObject(dc,old); DeleteObject(bitmap); DeleteDC(dc); ReleaseDC(page_,screen);
                }
            }
        }
    }
    check(NpcCombo::Generation()==generation,"changing library pages never saves or edits nodes");
    // Real range handlers, direct calls only: no mouse messages, focus or capture.
    SetWindowPos(host_,nullptr,0,0,980,720,SWP_NOMOVE|SWP_NOZORDER|SWP_NOACTIVATE);
    SetWindowPos(page_,nullptr,0,0,980,720,SWP_NOMOVE|SWP_NOZORDER|SWP_NOACTIVATE);
    UiI18n::SetLanguage(UiI18n::Language::SimplifiedChinese); Library(0,2);
    for (int zoom:{75,100,150}) {
        ZoomTo(zoom); Show(trim_index);
        const auto point=[&](double t) { const RECT r=TrimBarBox(trim_index); const int inset=static_cast<int>(std::lround(6.0*zoom/100)); return POINT{r.left+inset+static_cast<LONG>(std::lround((r.right-r.left-2*inset)*t)),(r.top+r.bottom)/2}; };
        const auto drag_edge=[&](double at,double to) {
            const POINT begin=point(at),end=point(to); check(TrimBegin(trim_index,begin),"range bar hit starts a drag at every zoom");
            const auto before=NpcCombo::Generation();
            for (int i=1;i<=400;++i) TrimMove({begin.x+(end.x-begin.x)*i/400,end.y});
            check(NpcCombo::Generation()==before,"400 pointer updates never save during a drag");
            TrimPreviewAt(trim_index);
            check(!preview_shown_.empty()&&preview_from_==TrimValue(trim_index,false)&&preview_to_==TrimValue(trim_index,true),"preview receives current range while dragging");
            TrimEnd(end); check(NpcCombo::Generation()==before+1,"changed range saves exactly once on release");
            const auto saved=NpcCombo::Load(moves.profile);
            check(saved.chain[static_cast<size_t>(trim_index)].trim_from==TrimValue(trim_index,false)&&saved.chain[static_cast<size_t>(trim_index)].trim_to==TrimValue(trim_index,true),"saved endpoints match released range");
        };
        // Reset each edge, then test crossing and clamping outside the bar.
        drag_edge(TrimValue(trim_index,false)/4000.0,-0.2);
        drag_edge(TrimValue(trim_index,true)>0?TrimValue(trim_index,true)/4000.0:1.0,1.2);
        check(TrimValue(trim_index,false)==0&&TrimValue(trim_index,true)==0,"drag past edges gives full range with to=0 sentinel");
        drag_edge(0,0.25); check(TrimValue(trim_index,false)==1000,"start handle reaches snapped time");
        drag_edge(1,0.75); check(TrimValue(trim_index,true)==3000,"end handle reaches snapped time");
        drag_edge(0.25,1.1); check(TrimValue(trim_index,false)==2950,"start cannot cross end");
        drag_edge(2950.0/4000,0.25);
        const int zoom_before=Zoom(),end_before=TrimValue(trim_index,true);
        check(ValueWheel(HoldValuePoint(trim_index),-1),"wheel over end field is handled");
        check(TrimValue(trim_index,true)==end_before-50&&Zoom()==zoom_before,"end wheel changes 50 ms without zooming canvas");
        check(ValueWheel(HoldValuePoint(trim_index),1)&&TrimValue(trim_index,true)==end_before,"wheel reverses end edit");
        TrimPreviewAt(trim_index); check(preview_from_==1000&&preview_to_==3000,"hover preview receives wheel-updated range");
    }
    ZoomTo(100); Show(trim_index);
    {
        const RECT box=TrimBarBox(trim_index); const POINT handle{box.left+6+(box.right-box.left-12)/4,(box.top+box.bottom)/2};
        const auto before=NpcCombo::Generation(); check(TrimBegin(trim_index,handle),"click existing handle accepted"); TrimEnd(handle);
        check(NpcCombo::Generation()==before,"clicking a handle without moving does not save");
        for (int i=0;i<20;++i) ValueWheel(HoldValuePoint(trim_index),1);
        check(TrimValue(trim_index,true)==0,"wheel up to clip end uses full-length sentinel");
        ValueWheel(HoldValuePoint(trim_index),1); check(TrimValue(trim_index,true)==0,"wheel cannot wrap beyond full length");
        ValueWheel(HoldValuePoint(trim_index),-1); check(TrimValue(trim_index,true)==3950,"wheel down from full length starts at clip minus one step");
        TrimPreviewAt(trim_index);
    }
    if (argc>1) {
        RECT area{}; GetClientRect(page_,&area);
        HDC screen=GetDC(page_),dc=CreateCompatibleDC(screen);
        HBITMAP bitmap=CreateCompatibleBitmap(screen,area.right,area.bottom); const HGDIOBJ old=SelectObject(dc,bitmap);
        SendMessageW(page_,WM_PRINTCLIENT,reinterpret_cast<WPARAM>(dc),PRF_CLIENT);
        const fs::path path=fs::path(argv[1])/L"trim_controls.png";
        { Gdiplus::Bitmap image(bitmap,nullptr); CLSID png{}; check(CLSIDFor(L"image/png",&png)&&image.Save(path.c_str(),&png,nullptr)==Gdiplus::Ok,"trim handles render without activation"); }
        SelectObject(dc,old); DeleteObject(bitmap); DeleteDC(dc); ReleaseDC(page_,screen);
    }
    check(!fs::exists(root/L"default.combo"),"default was never created or touched");
    RECT clip_after{}; GetClipCursor(&clip_after);
    check(EqualRect(&clip_before,&clip_after)!=FALSE,"cursor clip unchanged");
    // Foreground changes by the user are permitted; this process must never own it.
    DWORD foreground_pid=0; GetWindowThreadProcessId(GetForegroundWindow(),&foreground_pid);
    check(foreground_pid!=GetCurrentProcessId(),"test never takes foreground");
    std::fprintf(stderr,"{\"checks\":%d,\"failures\":%d,\"widths\":[720,980,1440],\"languages\":2,\"primary_tabs\":2,\"creation_tools\":27,\"hidden\":true,\"foreground_unchanged\":%s}\n",checks,failures,GetForegroundWindow()==foreground?"true":"false");
    ComboMenu::Shutdown(); DestroyWindow(host_); WeaponIcons::Shutdown(); UiTheme::Shutdown();
    for (const auto& entry:fs::directory_iterator(root)) if (entry.is_regular_file()) fs::remove(entry.path());
    fs::remove(root); return failures?1:0;
}
