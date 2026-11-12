// Reuse only the offline game stubs. The interactive preview entry point is never called.
#define wmain UnusedInteractivePreviewEntry
#include "combo_menu_preview.cpp"
#undef wmain
#include "combo_file.h"
#include "settings_store.h"
#include "weapon_icons.h"
#include "bone_motion_page.h"
#include <filesystem>
#include <commctrl.h>

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
    NpcCombo::Node bones=NpcCombo::ModifierNode(NpcCombo::NodeType::BoneTransform,1400,1200);bones.bones=BoneMotion::Feminine();
    const int bone_index=static_cast<int>(config.chain.size());config.chain.push_back(bones);
    check(NpcCombo::LinkModifier(config,boss_index,bone_index),"bone fixture linked to real boss ability");
    auto action=NpcCombo::WeaponActionsNode(100,1600);action.weapon="Long";action.routes.resize(2);action.routes[0].button=static_cast<int>(NpcCombo::Trigger::Heavy);action.routes[0].hold_ms=500;action.routes[0].next=0;
    action.routes[1].button=static_cast<int>(NpcCombo::Trigger::Keys);action.routes[1].keys={81,69};action.routes[1].chord=true;action.routes[1].next=1;
    const int action_index=static_cast<int>(config.chain.size());config.chain.push_back(action);
    NpcCombo::Node raw;raw.type=NpcCombo::NodeType::Trigger;raw.trigger=static_cast<int>(NpcCombo::Trigger::Keys);raw.x=100;raw.y=1900;
    const int raw_index=static_cast<int>(config.chain.size());config.chain.push_back(raw);
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
                    for (int button=0;button<38;++button) {
                        const int owner=ToolCategory(button); if (owner<0) continue;
                        const RECT box=ToolBox(button);
                        if (box.right>box.left) { check(owner==category&&contains(rail,box),"visible node belongs to selected category and fits"); visible_tools.insert(button); }
                        else if (owner!=category) check(box.right==box.left,"other categories cannot paint or receive a click");
                    }
                    LibraryTurn(1);
                }
                check(LibraryPage()==pages-1,"last page cannot overshoot"); LibraryTurn(-1000); check(LibraryPage()==0,"first page cannot undershoot");
            }
            check(visible_tools.size()==28&&visible_tools.count(37),"all 28 creation tools including bones remain reachable");
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
    {  // 招式来源: another boss's moves are added as its BOSS 技能; this table's own stay 当前外观招式
        UiI18n::SetLanguage(UiI18n::Language::SimplifiedChinese);
        SetWindowPos(host_,nullptr,0,0,1440,720,SWP_NOMOVE|SWP_NOZORDER|SWP_NOACTIVATE);
        SetWindowPos(page_,nullptr,0,0,1440,720,SWP_NOMOVE|SWP_NOZORDER|SWP_NOACTIVATE);
        const std::string own=EditedProfile();std::string other;NpcRetarget::SkinMoves other_moves;
        for (const NpcRetarget::Entry& e:NpcRetarget::GetSnapshot().entries)
            if (e.id!=own&&NpcRetarget::GetSkinMoves(e.index,other_moves)&&other_moves.skills.size()>1) { other=e.id; break; }
        check(!other.empty(),"a second boss with moves exists for the source test");
        Library(0,2); check(LibraryFromBox().right==LibraryFromBox().left,"招式来源 only shows in the BOSS 招式 list");
        Library(0,4); LibraryFrom("");
        const RECT rail=LibraryBox(),box=LibraryFromBox();
        check(contains(rail,box),"招式来源 dropdown fits the right library");
        NpcRetarget::SkinMoves own_moves; bool own_row=false;
        for (const NpcRetarget::Entry& e:NpcRetarget::GetSnapshot().entries) if (e.id==own) own_row=NpcRetarget::GetSkinMoves(e.index,own_moves);
        if (!own_row) NpcRetarget::GetSkinMoves(-1,own_moves);  // the hero's table
        check(LibraryMoves().size()==own_moves.skills.size()+own_moves.dodges.size(),"own source lists the table's skills and dodges");
        LibraryFrom(other);
        check(LibraryMoves()==other_moves.skills,"another boss's source lists exactly its skills");
        const auto before=NpcCombo::Generation();
        const int added=AddLibraryMove(1);
        check(added>=0&&NodeSummary(added)==std::to_string(static_cast<int>(NpcCombo::NodeType::BossSkill))+"|"+other+"|"+other_moves.skills[1],
              "a move of another boss becomes that boss's BOSS 技能");
        check(NpcCombo::Generation()>before,"adding a move saves the chain");
        LibraryFrom(own);  // the table's own look picked: its own moves again
        check(LibraryMoves().size()==own_moves.skills.size()+own_moves.dodges.size(),"picking the table's own look lists its moves");
        const int own_added=AddLibraryMove(0);
        check(own_added>=0&&NodeSummary(own_added).rfind(std::to_string(static_cast<int>(NpcCombo::NodeType::Skill))+"|",0)==0,"a move of the table's look stays a 当前外观招式");
        LibraryFrom("no-such-look"); check(LibraryMoves().size()==own_moves.skills.size()+own_moves.dodges.size(),"an unknown look falls back to the table's moves");
        LibraryFrom("");
    }
    BonePreviewAt(bone_index);
    check(BoneMotion::Encode(preview_bones_)==BoneMotion::Encode(bones.bones),"bone hover passes every row to skill preview");
    for(int height:{480,720}){
        SetWindowPos(host_,nullptr,0,0,980,height,SWP_NOMOVE|SWP_NOZORDER|SWP_NOACTIVATE);
        BoneEditor(bone_index);
        HWND editor=FindWindowExW(host_,nullptr,L"PBZBoneMotionPage",nullptr);
        check(editor&&BoneMotionPage::EditedNode()==bone_index&&!IsWindowVisible(editor),"node detail opens under hidden host");
        if(!editor)continue;
        HWND bone_combo=GetDlgItem(editor,808),search=GetDlgItem(editor,811);
        check((GetWindowLongPtrW(bone_combo,GWL_STYLE)&CBS_DROPDOWNLIST)==CBS_DROPDOWNLIST,"bone selector uses supported custom-painted dropdown style");
        check(SendMessageW(bone_combo,CB_GETCOUNT,0,0)==5,"body bone catalog excludes facial and jaw bones");
        SetWindowTextW(search,L"BREAST");check(SendMessageW(bone_combo,CB_GETCOUNT,0,0)==1,"bone search ignores case");
        SetWindowTextW(search,L"FACIAL");check(SendMessageW(bone_combo,CB_GETCOUNT,0,0)==0,"search cannot expose expression bones");
        SetWindowTextW(search,L"");
        SendMessageW(editor,WM_COMMAND,817,0);check((GetWindowLongPtrW(GetDlgItem(editor,819),GWL_STYLE)&WS_VISIBLE)!=0&&!(GetWindowLongPtrW(bone_combo,GWL_STYLE)&WS_VISIBLE),"simplified body panel switches without destroying detailed data");
        check(SendMessageW(GetDlgItem(editor,819),CB_GETCOUNT,0,0)>0&&GetDlgItem(editor,860),"simplified panel lists matching groups and real slider");
        {
            HWND groups=GetDlgItem(editor,819),uniform=GetDlgItem(editor,895);int whole=-1;
            for(int i=0;i<SendMessageW(groups,CB_GETCOUNT,0,0);++i)if(SendMessageW(groups,CB_GETITEMDATA,i,0)==14)whole=i;
            check(whole>=0&&uniform&&(GetWindowLongPtrW(uniform,GWL_STYLE)&WS_VISIBLE),"body tab exposes whole-character group and uniform slider");
            SendMessageW(groups,CB_SETCURSEL,whole,0);SendMessageW(editor,WM_COMMAND,MAKEWPARAM(819,CBN_SELCHANGE),reinterpret_cast<LPARAM>(groups));
            const auto before=NpcCombo::Generation();SendMessageW(uniform,WM_KEYDOWN,VK_END,0);
            const auto body=NpcCombo::Load(moves.profile).chain[static_cast<size_t>(bone_index)].bones;
            const auto root_row=std::find_if(body.rows.begin(),body.rows.end(),[](const auto& row){return row.bone=="root";});
            check(NpcCombo::Generation()==before+1&&root_row!=body.rows.end()&&root_row->scale==std::array<double,3>{3,3,3},"whole-body uniform slider commits only one root row");
            check(body.rows[0].motion==2&&body.rows[3].motion==1,"body scaling preserves feminine and jiggle rows");
            SetWindowTextW(GetDlgItem(editor,845),L"1.250");SendMessageW(editor,WM_COMMAND,820,0);
            const auto precise=NpcCombo::Load(moves.profile).chain[static_cast<size_t>(bone_index)].bones;
            const auto changed=std::find_if(precise.rows.begin(),precise.rows.end(),[](const auto& row){return row.bone=="root";});
            check(changed!=precise.rows.end()&&changed->scale==std::array<double,3>{1.25,1.25,1.25},"body uniform numeric input updates XYZ");
            check(!IsWindowVisible(editor)&&!GetCapture(),"body slider test stays hidden without capture");
        }
        SendMessageW(editor,WM_COMMAND,818,0);check((GetWindowLongPtrW(bone_combo,GWL_STYLE)&WS_VISIBLE)!=0,"detailed panel returns with bone selector");
        SendMessageW(editor,WM_COMMAND,827,0);
        HWND debug=GetDlgItem(editor,812);SendMessageW(debug,BM_SETCHECK,BST_CHECKED,0);SendMessageW(editor,WM_COMMAND,MAKEWPARAM(812,BN_CLICKED),reinterpret_cast<LPARAM>(debug));
        check((GetWindowLongPtrW(GetDlgItem(editor,816),GWL_STYLE)&WS_VISIBLE)!=0,"full skeleton viewer appears in dedicated tab");
        {HWND view=GetDlgItem(editor,816);RECT r{};GetClientRect(view,&r);HDC screen=GetDC(nullptr),dc=CreateCompatibleDC(screen);HBITMAP bitmap=CreateCompatibleBitmap(screen,r.right,r.bottom);const auto old=SelectObject(dc,bitmap);
            SendMessageW(view,WM_PRINTCLIENT,reinterpret_cast<WPARAM>(dc),PRF_CLIENT);int pixels=0;
            for(int y=28;y<r.bottom-28;++y)for(int x=28;x<r.right-28;++x)if(GetPixel(dc,x,y)==UiTheme::Accent())++pixels;
            check(pixels>80,"custom skeleton viewer paints complete body connections in hidden GDI test");SelectObject(dc,old);DeleteObject(bitmap);DeleteDC(dc);ReleaseDC(nullptr,screen);}
        SendMessageW(debug,BM_SETCHECK,BST_UNCHECKED,0);SendMessageW(editor,WM_COMMAND,MAKEWPARAM(812,BN_CLICKED),reinterpret_cast<LPARAM>(debug));
        SendMessageW(editor,WM_COMMAND,818,0);
        RECT bounds{},parent{};GetWindowRect(editor,&bounds);MapWindowPoints(nullptr,host_,reinterpret_cast<POINT*>(&bounds),2);GetClientRect(host_,&parent);
        check(contains(parent,bounds),"detail fits short and regular parent height");
        HWND tx=GetDlgItem(editor,830),apply=GetDlgItem(editor,807);
        {SendMessageW(editor,WM_COMMAND,900,0);HWND slider=GetDlgItem(editor,880);wchar_t slider_class[64]{};GetClassNameW(slider,slider_class,64);check(!wcscmp(slider_class,L"PBZBoneSlider"),"transform slider is custom drawn");
            const auto gen=NpcCombo::Generation();RECT r{};GetClientRect(slider,&r);
            SendMessageW(slider,WM_LBUTTONDOWN,MK_LBUTTON,MAKELPARAM(8,10));
            for(int n=0;n<400;++n)SendMessageW(slider,WM_MOUSEMOVE,MK_LBUTTON,MAKELPARAM(n%r.right,10));
            check(NpcCombo::Generation()==gen,"400 bone drag updates never save node");
            SendMessageW(slider,WM_LBUTTONUP,0,MAKELPARAM(r.right+80,10));
            check(NpcCombo::Generation()==gen+1&&NpcCombo::Load(moves.profile).chain[static_cast<size_t>(bone_index)].bones.rows[0].translation[0]==100,"bone drag clamps and commits once");
            SendMessageW(slider,WM_KEYDOWN,VK_HOME,0);check(NpcCombo::Load(moves.profile).chain[static_cast<size_t>(bone_index)].bones.rows[0].translation[0]==-100,"slider keyboard home reaches lower bound");
            check(!GetCapture(),"hidden bone slider never captures real mouse");}
        {const auto unchanged=NpcCombo::Generation();for(int tab:{920,921,818})SendMessageW(editor,WM_COMMAND,tab,0);check(NpcCombo::Generation()==unchanged,"presets and status tabs never save nodes");
            HWND precision=GetDlgItem(editor,922);SendMessageW(precision,BM_SETCHECK,BST_CHECKED,0);SendMessageW(editor,WM_COMMAND,922,0);check((GetWindowLongPtrW(GetDlgItem(editor,836),GWL_STYLE)&WS_VISIBLE)!=0,"precision input option exposes scale fields");
            SendMessageW(precision,BM_SETCHECK,BST_UNCHECKED,0);SendMessageW(editor,WM_COMMAND,922,0);check(!(GetWindowLongPtrW(GetDlgItem(editor,836),GWL_STYLE)&WS_VISIBLE),"numeric values remain inside slider by default");
            const auto before_uniform=NpcCombo::Generation();SendMessageW(GetDlgItem(editor,894),WM_KEYDOWN,VK_END,0);const auto uniform=NpcCombo::Load(moves.profile).chain[static_cast<size_t>(bone_index)].bones;
            check(NpcCombo::Generation()==before_uniform+1&&uniform.rows[0].scale==std::array<double,3>{3,3,3},"uniform size slider commits all three axes once");
            SendMessageW(editor,WM_COMMAND,912,0);const auto reset_scale=NpcCombo::Load(moves.profile).chain[static_cast<size_t>(bone_index)].bones;
            check(reset_scale.rows[0].scale==std::array<double,3>{1,1,1}&&reset_scale.rows[0].translation[0]==-100&&reset_scale.rows.size()==uniform.rows.size(),"scale reset preserves translation and other rows");
            SetWindowTextW(GetDlgItem(editor,844),L"1.250");SendMessageW(editor,WM_COMMAND,807,0);check(NpcCombo::Load(moves.profile).chain[static_cast<size_t>(bone_index)].bones.rows[0].scale==std::array<double,3>{1.25,1.25,1.25},"uniform precise input updates every scale axis");
            SendMessageW(editor,WM_COMMAND,912,0);}
        if(argc>1&&height==720){
            SendMessageW(editor,WM_VSCROLL,SB_TOP,0);
            RECT r{};GetClientRect(editor,&r);HDC screen=GetDC(nullptr),dc=CreateCompatibleDC(screen);HBITMAP bitmap=CreateCompatibleBitmap(screen,r.right,r.bottom);auto old=SelectObject(dc,bitmap);
            SendMessageW(editor,WM_PRINT,reinterpret_cast<WPARAM>(dc),PRF_CLIENT|PRF_CHILDREN|PRF_ERASEBKGND);
            {Gdiplus::Bitmap image(bitmap,nullptr);CLSID png{};const auto path=fs::path(argv[1])/L"bone_editor.png";check(CLSIDFor(L"image/png",&png)&&image.Save(path.c_str(),&png,nullptr)==Gdiplus::Ok,"bone editor renders without focus");}
            SelectObject(dc,old);DeleteObject(bitmap);DeleteDC(dc);ReleaseDC(nullptr,screen);}

        if(argc>2&&height==720){
            RECT original{};GetWindowRect(editor,&original);
            auto render=[&](const wchar_t* name,int tab,int h){
                SetWindowPos(editor,nullptr,0,0,960,h,SWP_NOMOVE|SWP_NOZORDER|SWP_NOACTIVATE);
                SendMessageW(editor,WM_COMMAND,tab,0);SendMessageW(editor,WM_VSCROLL,SB_TOP,0);
                RECT r{};GetClientRect(editor,&r);HDC screen=GetDC(nullptr),dc=CreateCompatibleDC(screen);
                HBITMAP bitmap=CreateCompatibleBitmap(screen,r.right,r.bottom);auto old=SelectObject(dc,bitmap);
                SendMessageW(editor,WM_PRINT,reinterpret_cast<WPARAM>(dc),PRF_CLIENT|PRF_CHILDREN|PRF_ERASEBKGND);
                {Gdiplus::Bitmap image(bitmap,nullptr);CLSID png{};const auto path=fs::path(argv[1])/name;
                check(CLSIDFor(L"image/png",&png)&&image.Save(path.c_str(),&png,nullptr)==Gdiplus::Ok,"full editor export");}
                SelectObject(dc,old);DeleteObject(bitmap);DeleteDC(dc);ReleaseDC(nullptr,screen);
            };
            SendMessageW(editor,WM_COMMAND,901,0);SendMessageW(editor,WM_COMMAND,903,0);
            render(L"bones_full.png",818,1350);
            render(L"body_full.png",817,720);render(L"presets_full.png",920,480);
            render(L"status_full.png",921,720);render(L"debug_full.png",827,720);
            SendMessageW(editor,WM_COMMAND,818,0);SendMessageW(editor,WM_COMMAND,901,0);SendMessageW(editor,WM_COMMAND,903,0);
            SetWindowPos(editor,nullptr,0,0,original.right-original.left,original.bottom-original.top,SWP_NOMOVE|SWP_NOZORDER|SWP_NOACTIVATE);
        }
        SetWindowTextW(tx,L"2.5");
        const auto before=NpcCombo::Generation();SendMessageW(editor,WM_COMMAND,MAKEWPARAM(807,BN_CLICKED),reinterpret_cast<LPARAM>(apply));
        check(NpcCombo::Generation()==before+1&&NpcCombo::Load(moves.profile).chain[static_cast<size_t>(bone_index)].bones.rows[0].translation[0]==2.5,"detail apply saves selected row through real node callback");
        BonePreviewAt(bone_index);check(preview_bones_.rows[0].translation[0]==2.5,"edited bone values update preview");
        SetWindowTextW(tx,L"nan");const auto valid=NpcCombo::Generation();SendMessageW(editor,WM_COMMAND,MAKEWPARAM(807,BN_CLICKED),reinterpret_cast<LPARAM>(apply));
        check(NpcCombo::Generation()==valid,"invalid row cannot overwrite saved values");
        SCROLLINFO si{sizeof(si),SIF_RANGE|SIF_PAGE};GetScrollInfo(editor,SB_VERT,&si);check(si.nMax>static_cast<int>(si.nPage),"short detail exposes vertical scrolling");
        for(int i=0;i<30;++i)SendMessageW(editor,WM_VSCROLL,SB_LINEDOWN,0);
        RECT button{},client{};GetWindowRect(apply,&button);MapWindowPoints(nullptr,editor,reinterpret_cast<POINT*>(&button),2);GetClientRect(editor,&client);
        check(contains(client,button),"scrolling reaches apply button");
        BoneMotionPage::Close();check(BoneMotionPage::EditedNode()==-1,"detail closes cleanly");
    }
    { HWND global=BoneMotionPage::CreatePage(host_);check(global!=nullptr,"global bone page creates independently of node editor");
      for(int i=0;i<100;++i){SendMessageW(global,WM_COMMAND,static_cast<WPARAM>(i%2?818:921),0);SendMessageW(global,WM_SIZE,0,0);}
      check(IsWindow(global)!=FALSE,"global navigation survives repeated layout");
      SendMessageW(global,WM_COMMAND,818,0);int visibility_changes=0;
      HWND slider=GetDlgItem(global,886);SetWindowSubclass(slider,[](HWND w,UINT m,WPARAM a,LPARAM b,UINT_PTR,DWORD_PTR data)->LRESULT{if(m==WM_SHOWWINDOW)++*reinterpret_cast<int*>(data);return DefSubclassProc(w,m,a,b);},771,reinterpret_cast<DWORD_PTR>(&visibility_changes));
      for(int i=0;i<100;++i)SendMessageW(global,WM_SIZE,0,0);
      check(visibility_changes==0,"stable layout never hides and re-shows scale slider");BoneMotionPage::Shutdown(); }
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
        {
            const auto point=[&](double t){const RECT bar=TrimBarBox(trim_index);return POINT{bar.left+6+static_cast<LONG>(std::lround((bar.right-bar.left-12)*t)),(bar.top+bar.bottom)/2};};
            const POINT first=point(.5),second=point(.8),moved=point(.6);
            SendMessageW(page_,WM_RBUTTONDOWN,MK_RBUTTON,MAKELPARAM(first.x,first.y));SendMessageW(page_,WM_RBUTTONUP,0,MAKELPARAM(first.x,first.y));
            check(TrimHoldPoints(trim_index)==std::vector<int>{2000},"actual timeline right-click adds a charge point instead of opening search");
            check(TrimHoldToggle(trim_index,second)&&TrimHoldPoints(trim_index)==std::vector<int>{2000,3200},"multiple charge markers are supported");
            const auto prior_save=NpcCombo::Generation();check(TrimBegin(trim_index,first),"charge marker starts a drag");TrimMove(moved);
            check(NpcCombo::Generation()==prior_save,"marker drag previews without saving");TrimEnd(moved);
            check(TrimHoldPoints(trim_index)==std::vector<int>{2400,3200}&&NpcCombo::Generation()==prior_save+1,"marker drag commits its new authored time once");
            TrimPreviewAt(trim_index);check(preview_holds_==std::vector<int>{2400,3200},"preview receives all charge points");
            check(TrimHoldToggle(trim_index,moved)&&TrimHoldToggle(trim_index,second)&&TrimHoldPoints(trim_index).empty(),"right-clicking markers removes them");
            check(TrimHoldToggle(trim_index,first)&&TrimHoldToggle(trim_index,second),"charge markers restored for rendered inspection");
        }
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
    {const auto before=NpcCombo::Generation();const RECT canvas=Canvas();SearchAt({canvas.left+20,canvas.top+20});HWND input=SearchInput();
        wchar_t input_class[32]{};GetClassNameW(input,input_class,32);check(input&&!wcscmp(input_class,L"Edit"),"search is a native edit with IME and selection support");
        SendMessageW(input,WM_CHAR,L'加',1);SendMessageW(input,WM_CHAR,L'速',1);check(SearchQuery()==L"加速"&&SearchRows()>0,"Chinese text reaches real search filtering");
        SendMessageW(input,EM_SETSEL,0,-1);SendMessageW(input,EM_REPLACESEL,FALSE,reinterpret_cast<LPARAM>(L"pelvis"));check(SearchQuery()==L"pelvis","replacement text synchronizes search");
        SendMessageW(input,WM_CHAR,VK_BACK,1);check(SearchQuery()==L"pelvi","native backspace edits the query once");
        MSG msg{};msg.hwnd=input;msg.message=WM_CHAR;msg.wParam=L's';check(ComboMenu::OwnsTextMessage(msg),"hub bypasses dialog traversal for search input");
        DispatchMessageW(&msg);check(SearchQuery()==L"pelvis","routed character is dispatched once");
        SendMessageW(input,WM_KEYDOWN,VK_ESCAPE,0);check(!SearchOpen()&&!(GetWindowLongPtrW(input,GWL_STYLE)&WS_VISIBLE),"escape closes the search edit");
        check(!ComboMenu::OwnsTextMessage(msg),"closed search stops claiming messages");check(NpcCombo::Generation()==before,"search editing never changes the saved chain");}
    ZoomTo(63);Show(action_index);const POINT held=RouteHoldPoint(action_index,0);check(ValueWheel(held,1),"weapon action hold stepper accepts wheel");
    check(NpcCombo::Load(moves.profile).chain[static_cast<size_t>(action_index)].routes[0].hold_ms==600,"weapon action hold value persists");
    check(RouteKeys(action_index,1).x>=0&&RouteMode(action_index,1).x>=0,"weapon special keys and mode have real UI hit boxes");
    RoutePicker(action_index,1,2);check(SearchOpen()&&SearchRows()>0,"weapon row opens key mode picker instead of action list");SendMessageW(SearchInput(),WM_KEYDOWN,VK_ESCAPE,0);
    // Real page procedure and hub recording interception, sent only to our hidden host.
    // No hardware injection, focus change, capture, or writes outside the temporary profile.
    const auto recordingMessage=[&](UINT type,WPARAM key=0,LPARAM detail=0){MSG msg{};msg.hwnd=host_;msg.message=type;msg.wParam=key;msg.lParam=detail;return ComboMenu::HandleRecordingMessage(msg);};
    const auto savedKeys=[&](){return NpcCombo::Load(moves.profile).chain[static_cast<size_t>(raw_index)].keys;};
    const auto mouseFive=[&](){
        for(UINT down:{WM_LBUTTONDOWN,WM_RBUTTONDOWN,WM_MBUTTONDOWN})check(recordingMessage(down),"mouse button over hub consumed by recorder");
        check(recordingMessage(WM_XBUTTONDOWN,MAKEWPARAM(0,XBUTTON1)),"side button one captured");
        check(recordingMessage(WM_XBUTTONDOWN,MAKEWPARAM(0,XBUTTON2)),"side button two captured");
    };
    const std::vector<int> mouseKeys{VK_LBUTTON,VK_RBUTTON,VK_MBUTTON,VK_XBUTTON1,VK_XBUTTON2};
    auto generationBefore=NpcCombo::Generation();RecordAt(raw_index);mouseFive();
    check(RecordingKeys()==mouseKeys&&savedKeys().empty()&&NpcCombo::Generation()==generationBefore,"five mouse buttons only change unsaved draft");
    check(recordingMessage(WM_KEYDOWN,VK_ESCAPE)&&RecordingNode()<0&&savedKeys().empty()&&NpcCombo::Generation()==generationBefore,"Esc cancels without saving or hiding hub");
    RecordAt(raw_index);mouseFive();recordingMessage(WM_KEYDOWN,'Q');recordingMessage(WM_KEYDOWN,'Q',LPARAM{1}<<30);recordingMessage(WM_KEYDOWN,'E');
    auto six=mouseKeys;six.push_back('Q');
    check(RecordingKeys()==six&&RecordingNode()==raw_index,"six-key capacity waits for confirmation and ignores repeat/overflow");
    recordingMessage(WM_KEYDOWN,VK_RETURN,LPARAM{1}<<30);
    check(RecordingNode()==raw_index,"held Enter repeat cannot accidentally confirm");
    recordingMessage(WM_KEYDOWN,VK_RETURN);
    check(RecordingNode()<0&&savedKeys()==six&&NpcCombo::Generation()==generationBefore+1,"Enter commits all six keys in exactly one save");
    check(NpcCombo::Decode(NpcCombo::Encode(NpcCombo::Load(moves.profile))).chain[static_cast<size_t>(raw_index)].keys==six,"all mouse buttons survive normal profile serialization");
    const auto originalThen=NpcCombo::Load(moves.profile).chain[static_cast<size_t>(raw_index)].then;generationBefore=NpcCombo::Generation();
    RecordAt(raw_index,-5);recordingMessage(WM_RBUTTONDOWN);recordingMessage(WM_KEYDOWN,'E');
    check(RecordingKeys()==std::vector<int>{'E'}&&RecordingNode()==raw_index,"continue-key draft replaces its candidate but does not auto-commit");
    recordingMessage(WM_KEYDOWN,VK_ESCAPE);
    check(NpcCombo::Load(moves.profile).chain[static_cast<size_t>(raw_index)].then==originalThen&&NpcCombo::Generation()==generationBefore,"cancel restores original continue key with no write");
    RecordAt(raw_index,-5);recordingMessage(WM_LBUTTONDOWN);recordingMessage(WM_KEYDOWN,VK_RETURN);
    check(NpcCombo::Load(moves.profile).chain[static_cast<size_t>(raw_index)].then==NpcCombo::kThenRaw+VK_LBUTTON&&savedKeys()==six,"left mouse can be the continue key without changing trigger keys");
    generationBefore=NpcCombo::Generation();RecordAt(action_index,1);recordingMessage(WM_RBUTTONDOWN);recordingMessage(WM_KEYDOWN,VK_RETURN);
    check(NpcCombo::Load(moves.profile).chain[static_cast<size_t>(action_index)].routes[1].keys==std::vector<int>{'Q','E',VK_RBUTTON}&&NpcCombo::Generation()==generationBefore+1,"weapon derivation custom row shares transactional mouse capture");
    generationBefore=NpcCombo::Generation();RecordAt(raw_index,-5);recordingMessage(WM_MBUTTONDOWN);ComboMenu::Tick();
    check(RecordingNode()<0&&NpcCombo::Generation()==generationBefore,"hidden page cancels its draft without saving");
    RecordAt(raw_index,-5);recordingMessage(WM_RBUTTONDOWN);auto external=NpcCombo::Load(moves.profile);external.chain[static_cast<size_t>(raw_index)].then=NpcCombo::kThenRaw+'Z';NpcCombo::Save(moves.profile,external);generationBefore=NpcCombo::Generation();recordingMessage(WM_KEYDOWN,VK_RETURN);
    check(RecordingNode()<0&&NpcCombo::Generation()==generationBefore&&NpcCombo::Load(moves.profile).chain[static_cast<size_t>(raw_index)].then==NpcCombo::kThenRaw+'Z',"external profile edit invalidates recording instead of overwriting it");
    check(!ComboMenu::HandleRecordingMessage(MSG{}),"recording interceptor stops consuming after completion");
    check(!fs::exists(root/L"default.combo"),"default was never created or touched");
    RECT clip_after{}; GetClipCursor(&clip_after);
    check(EqualRect(&clip_before,&clip_after)!=FALSE,"cursor clip unchanged");
    check(ToolBox(38).right>ToolBox(38).left&&ToolBox(39).right>ToolBox(39).left,"copy and paste self-painted buttons are reachable above canvas");
    // Clipboard uses only hidden probes and isolated profile writes, never the OS clipboard.
    RefreshData();PickNodes({boss_index,trim_index,bone_index,NpcCombo::KeyTrigger(NpcCombo::Load(moves.profile))});
    const auto clipboardGeneration=NpcCombo::Generation();const auto clipboardSize=NpcCombo::Load(moves.profile).chain.size();
    check(ClipboardShortcut('C')&&ClipboardCount()==3&&NpcCombo::Generation()==clipboardGeneration,"copy captures linked subgraph and skips singleton without writing");
    check(ClipboardShortcut('V')&&NpcCombo::Generation()==clipboardGeneration+1,"paste commits once");
    auto pasted=NpcCombo::Load(moves.profile);const int pastedBoss=static_cast<int>(clipboardSize);
    check(pasted.chain.size()==clipboardSize+3&&Picked()==3&&PickedNode(pastedBoss),"pasted nodes are selected together");
    check(pasted.chain[pastedBoss].mods[NpcCombo::kModTrim]==pastedBoss+1&&pasted.chain[pastedBoss].mods[NpcCombo::kModBone]==pastedBoss+2,"hidden canvas paste preserves trim and bone wires");
    check(ClipboardShortcut('V')&&NpcCombo::Load(moves.profile).chain.size()==clipboardSize+6,"repeated paste adds independent copy");
    auto duration=NpcCombo::Load(moves.profile);duration.chain[trim_index].trim_from=500;duration.chain[trim_index].trim_to=2500;NpcCombo::Save(moves.profile,duration);RefreshData();
    check(TrimDurationText(trim_index).find(L"2.00 s")!=std::wstring::npos,"trim displays kept duration, not original four second length");
    duration.chain[trim_index].trim_to=0;NpcCombo::Save(moves.profile,duration);RefreshData();check(TrimDurationText(trim_index).find(L"3.50 s")!=std::wstring::npos,"trim to natural end subtracts start");
    duration.chain[trim_index].trim_to=6000;NpcCombo::Save(moves.profile,duration);RefreshData();check(TrimDurationText(trim_index).find(L"3.50 s")!=std::wstring::npos,"trim end beyond source is clamped for duration");
    duration.chain[trim_index].trim_from=5000;NpcCombo::Save(moves.profile,duration);RefreshData();check(TrimDurationText(trim_index).find(L"4.00 s")!=std::wstring::npos,"out of range start reports actual whole-move fallback");
    duration.chain[trim_index].trim_from=500;duration.chain[trim_index].trim_to=0;
    NpcCombo::Node second=duration.chain[boss_index];second.ability="GA_Test_Clipboard_UnknownLength";const int secondBoss=static_cast<int>(duration.chain.size());duration.chain.push_back(second);
    NpcCombo::Save(moves.profile,duration);RefreshData();NpcCombo::Publish(moves.profile,true,secondBoss,L"test");RefreshData();
    check(TrimDurationText(trim_index).find(L"待读取")!=std::wstring::npos&&TrimSourceLabel(trim_index).find(L"UnknownLength")!=std::wstring::npos,"shared trim follows running source and never borrows another move length");
    NpcCombo::RecordClip(second.ability,6.0f);check(TrimDurationText(trim_index).find(L"5.50 s")!=std::wstring::npos,"new source length refreshes kept duration immediately");
    NpcCombo::Publish(moves.profile,false,-1,L"");RefreshData();PickNodes({boss_index});check(TrimDurationText(trim_index).find(L"3.50 s")!=std::wstring::npos,"shared trim follows selected linked move after playback ends");
    check(AnimationDurationText(boss_index).find(L"4.00 s")!=std::wstring::npos,"boss node displays original animation duration");
    check(AnimationDurationText(boss_index).find(L"3.50 s")!=std::wstring::npos,"boss node displays trimmed animation duration");
    duration.chain[boss_index].type=NpcCombo::NodeType::Skill;duration.chain[boss_index].boss.clear();
    NpcCombo::Save(moves.profile,duration);RefreshData();TrimPreviewAt(trim_index);
    check(preview_from_==500&&preview_to_==0,"current-character skill preview receives its trim");
    check(TrimDurationText(trim_index).find(L"3.50 s")!=std::wstring::npos,"current-character skill trim displays duration");
    // Exercise the actual round-pin drag -> popup -> create-and-connect path.
    SetWindowPos(page_,nullptr,0,0,1440,900,SWP_NOZORDER|SWP_NOACTIVATE);
    for(const auto type:{NpcCombo::NodeType::Skill,NpcCombo::NodeType::BossSkill,NpcCombo::NodeType::WeaponMove,NpcCombo::NodeType::AutoDodge}){
        NpcCombo::Config popup;NpcCombo::Node source;source.type=type;source.ability=moves.skills.front();source.boss=moves.profile;source.weapon="Long";source.x=80;source.y=100;
        popup.chain.push_back(source);NpcCombo::EnsureKey(popup);NpcCombo::EnsurePreInput(popup);NpcCombo::EnsureAirDefault(popup);
        for(size_t i=1;i<popup.chain.size();++i){popup.chain[i].x=2400;popup.chain[i].y=200+static_cast<int>(i)*300;}
        NpcCombo::Save(moves.profile,popup);RefreshData();ZoomTo(100);Show(0);
        const POINT start=ModOut(0);const RECT canvas=Canvas();const POINT drop{canvas.left+520,canvas.top+300};
        SendMessageW(page_,WM_LBUTTONDOWN,MK_LBUTTON,MAKELPARAM(start.x,start.y));
        SendMessageW(page_,WM_MOUSEMOVE,MK_LBUTTON,MAKELPARAM(drop.x,drop.y));
        SendMessageW(page_,WM_LBUTTONUP,0,MAKELPARAM(drop.x,drop.y));
        check(SearchOpen(),"round pin opens modifier popup");HWND input=SearchInput();
        SendMessageW(input,WM_SETTEXT,0,reinterpret_cast<LPARAM>(L"裁剪"));
        check(SearchRows()>0,"trim is listed for every supported skill type");
        SendMessageW(input,WM_KEYDOWN,VK_RETURN,0);
        const auto saved=NpcCombo::Load(moves.profile);const int popup_trim=NpcCombo::ModOf(saved,0,NpcCombo::kModTrim);
        check(popup_trim>=0&&saved.chain[static_cast<size_t>(popup_trim)].type==NpcCombo::NodeType::Trim,"popup creates and connects trim");
        check(NpcCombo::ModOf(NpcCombo::Decode(NpcCombo::Encode(saved)),0,NpcCombo::kModTrim)==popup_trim,"popup trim link survives persistence");
    }
    // Foreground changes by the user are permitted; this process must never own it.
    DWORD foreground_pid=0; GetWindowThreadProcessId(GetForegroundWindow(),&foreground_pid);
    check(foreground_pid!=GetCurrentProcessId(),"test never takes foreground");
    std::fprintf(stderr,"{\"checks\":%d,\"failures\":%d,\"widths\":[720,980,1440],\"languages\":2,\"primary_tabs\":2,\"creation_tools\":28,\"hidden\":true,\"foreground_unchanged\":%s}\n",checks,failures,GetForegroundWindow()==foreground?"true":"false");
    ComboMenu::Shutdown(); DestroyWindow(host_); WeaponIcons::Shutdown(); UiTheme::Shutdown();
    for (const auto& entry:fs::directory_iterator(root)) if (entry.is_regular_file()) fs::remove(entry.path());
    fs::remove(root); return failures?1:0;
}
