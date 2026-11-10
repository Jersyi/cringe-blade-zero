// Export the real custom-painted controls in a hidden offline host. No game access.
#define wmain UnusedInteractivePreviewEntry
#include "combo_menu_preview.cpp"
#undef wmain
#include "settings_store.h"
#include "bone_motion_page.h"
#include "weapon_icons.h"
#include <filesystem>
#include <fstream>

namespace {
namespace fs=std::filesystem;
void Capture(HWND window,const fs::path& path,const RECT* crop=nullptr){
    RECT area{};GetClientRect(window,&area);
    HDC screen=GetDC(window),dc=CreateCompatibleDC(screen);
    HBITMAP bitmap=CreateCompatibleBitmap(screen,area.right,area.bottom);const auto old=SelectObject(dc,bitmap);
    SendMessageW(window,WM_PRINT,reinterpret_cast<WPARAM>(dc),PRF_CLIENT|PRF_CHILDREN|PRF_ERASEBKGND);
    SendMessageW(window,WM_PRINTCLIENT,reinterpret_cast<WPARAM>(dc),PRF_CLIENT);
    {Gdiplus::Bitmap image(bitmap,nullptr);CLSID png{};
     if(!CLSIDFor(L"image/png",&png))std::exit(3);
     if(crop){
         if(crop->left<0||crop->top<0||crop->right>area.right||crop->bottom>area.bottom)std::exit(4);
         auto part=image.Clone(crop->left,crop->top,crop->right-crop->left,crop->bottom-crop->top,PixelFormat32bppARGB);
         if(!part||part->Save(path.c_str(),&png,nullptr)!=Gdiplus::Ok)std::exit(5);delete part;
     }else if(image.Save(path.c_str(),&png,nullptr)!=Gdiplus::Ok)std::exit(6);}
    SelectObject(dc,old);DeleteObject(bitmap);DeleteDC(dc);ReleaseDC(window,screen);
}
}

int wmain(int argc,wchar_t** argv){
    if(argc!=2)return 2;
    SetErrorMode(SEM_FAILCRITICALERRORS|SEM_NOGPFAULTERRORBOX);
    SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_UNAWARE);
    const HWND foreground=GetForegroundWindow();RECT clip{};GetClipCursor(&clip);
    const fs::path output=fs::absolute(argv[1]);fs::create_directories(output);
    const fs::path isolated=fs::temp_directory_path()/(L"pbz_manual_"+std::to_wstring(GetCurrentProcessId())+L"_"+std::to_wstring(GetTickCount64()));
    fs::create_directory(isolated);SettingsStore::Initialize((isolated/L"test.ini").c_str());
    NpcCombo::SetFolder(isolated.wstring());if(!NpcCombo::SaveAs(L"manual"))return 9;
    UiCursor::SetHandsOff(true);UiTheme::Initialize();UiI18n::SetLanguage(UiI18n::Language::SimplifiedChinese);
    WNDCLASSW cls{};cls.lpfnWndProc=DefWindowProcW;cls.hInstance=GetModuleHandleW(nullptr);cls.lpszClassName=L"PBZManualHiddenHost";RegisterClassW(&cls);
    host_=CreateWindowExW(WS_EX_NOACTIVATE|WS_EX_TOOLWINDOW,cls.lpszClassName,L"",WS_POPUP,0,0,1440,1000,nullptr,nullptr,cls.hInstance,nullptr);
    page_=ComboMenu::CreatePage(host_);SetWindowPos(page_,nullptr,0,0,1440,1000,SWP_NOZORDER|SWP_NOACTIVATE);
    using namespace NpcCombo;using namespace ComboMenu::Probe;
    NpcRetarget::SkinMoves moves;NpcRetarget::GetSkinMoves(0,moves);
    NpcCombo::RecordClip(moves.skills.front(),4.0f);
    constexpr const char* names[]={"Skill","Interval","Trigger","AutoDodge","Branch","Speed","Weapon","Armor","Interrupt","Grab","BossSkill","WeaponMove","Damage","Group","Teleport","Invincible","Look","HeroLook","KeepLook","ForceHit","PreInput","WeaponActions","IgnoreGuard","KeepChain","Assist","Hook","Trim","Air","AirDefault","BoneTransform"};
    static_assert(std::size(names)==static_cast<size_t>(NodeType::BoneTransform)+1);
    const auto make=[&](NodeType type){
        Node n;n.type=type;n.x=200;n.y=180;n.ability=moves.skills.front();
        if(IsModifier(n))n=ModifierNode(type,200,180);
        if(type==NodeType::Trigger)n=TriggerNode(Trigger::NpcKey,200,180);
        if(type==NodeType::Interval)n.ms=500;
        if(type==NodeType::Grab)n.ability="MuXiaoKui:GA_MuXiaoKui_GEx_01";
        if(type==NodeType::Branch){n=BranchNode(200,180);n.routes={{static_cast<int>(Trigger::Light),0,1},{static_cast<int>(Trigger::Heavy),1000,1}};}
        if(type==NodeType::BossSkill||type==NodeType::Assist||type==NodeType::Look)n.boss=moves.profile;
        if(type==NodeType::WeaponMove){const auto& w=ComboWeaponsData::kWeapons[0];n.weapon=w.id;n.ability=w.moves[0].ability;}
        if(type==NodeType::WeaponActions){n=WeaponActionsNode(200,180);n.weapon=ComboWeaponsData::kWeapons[0].id;n.routes.resize(2);n.routes[0].next=1;n.routes[1].next=1;n.routes[1].hold_ms=500;}
        if(type==NodeType::Hook)n=HookNode(200,180);
        if(type==NodeType::Speed){n.curve=true;n.points={{0,80},{35,100},{70,250},{100,100}};}
        if(type==NodeType::Trim){n.trim_from=500;n.trim_to=3500;n.trim_holds={1500,2500};}
        if(type==NodeType::PreInput)n.ms=300;
        if(type==NodeType::BoneTransform)n.bones=BoneMotion::Feminine();
        if(type==NodeType::Group){n.w=460;n.h=240;}
        return n;
    };
    std::ofstream manifest(output/L"nodes.json");manifest<<"{\"offline\":true,\"real_ui_renderer\":true,\"nodes\":[";
    int count=0;
    const auto render=[&](Node n,const char* name){
        Config c;c.placed=true;c.chain.push_back(n);
        Node source=make(NodeType::BossSkill);source.x=950;source.y=160;c.chain.push_back(source);
        if(n.type==NodeType::Weapon)c.chain[1]=TriggerNode(Trigger::Light,950,160);
        if(IsModifier(n))LinkModifier(c,1,0);else if(!IsSetting(n))c.chain[0].next=1;
        if(n.type==NodeType::Group){c.chain[1].group=0;c.chain[1].x=240;c.chain[1].y=250;c.chain[0].next=-1;}
        EnsureKey(c);EnsurePreInput(c);EnsureAirDefault(c);
        for(size_t i=2;i<c.chain.size();++i){c.chain[i].x=950;c.chain[i].y=450+static_cast<int>(i)*180;}
        Save(moves.profile,c);RefreshData();ZoomTo(125);Library(0,2);
        Show(0);
        RECT r=NodeRect(0);InflateRect(&r,5,5);Capture(page_,output/(std::string("node_")+name+".png"),&r);
        if(count++)manifest<<",";manifest<<"\""<<name<<"\"";
        if(n.type==NodeType::BoneTransform){
            BoneEditor(0);HWND editor=FindWindowExW(host_,nullptr,L"PBZBoneMotionPage",nullptr);
            if(!editor||IsWindowVisible(editor))std::exit(7);
            SetWindowPos(editor,nullptr,0,0,1100,900,SWP_NOZORDER|SWP_NOACTIVATE);
            SendMessageW(editor,WM_COMMAND,817,0);Capture(editor,output/L"ui_body.png");
            SendMessageW(editor,WM_COMMAND,818,0);Capture(editor,output/L"ui_bones.png");BoneMotionPage::Close();
        }
        if(n.type==NodeType::Speed){
            SpeedCurveWindow::Host h;h.read=[&](std::vector<CurvePoint>& p,bool& on,int& speed,std::wstring& title){p=n.points;on=true;speed=150;title=L"加速曲线示例";return true;};h.write=[](const std::vector<CurvePoint>&,bool,bool){};
            SpeedCurveWindow::Open(page_,0,std::move(h));HWND w=SpeedCurveWindow::Window();if(!w||IsWindowVisible(w))std::exit(8);Capture(w,output/L"ui_curve.png");SpeedCurveWindow::Close();
        }
    };
    for(size_t i=0;i<std::size(names);++i)render(make(static_cast<NodeType>(i)),names[i]);
    render(AirSetNode(200,180),"AirSet");
    auto keys=TriggerNode(Trigger::Keys,200,180);keys.keys={VK_LBUTTON,VK_RBUTTON};keys.chord=true;render(keys,"Keys");
    auto combo=TriggerNode(Trigger::Combo,200,180);combo.keys={static_cast<int>(Trigger::Stance),static_cast<int>(Trigger::Special)};combo.chord=true;render(combo,"ComboKeys");
    render(TriggerNode(Trigger::Heavy,200,180),"ReplaceKey");
    Config overview;overview.placed=true;overview.chain={TriggerNode(Trigger::NpcKey,80,100),make(NodeType::BossSkill),make(NodeType::Trim),make(NodeType::Speed)};
    overview.chain[0].next=1;overview.chain[1].x=430;overview.chain[1].y=100;overview.chain[2].x=430;overview.chain[2].y=400;overview.chain[3].x=80;overview.chain[3].y=400;
    LinkModifier(overview,1,2);LinkModifier(overview,1,3);EnsurePreInput(overview);EnsureAirDefault(overview);
    overview.chain[static_cast<size_t>(PreInputNode(overview))].x=80;overview.chain[static_cast<size_t>(PreInputNode(overview))].y=680;
    overview.chain[static_cast<size_t>(AirDefaultNode(overview))].x=430;overview.chain[static_cast<size_t>(AirDefaultNode(overview))].y=680;
    Save(moves.profile,overview);RefreshData();ZoomTo(100);Show(0);
    Library(0,2);Capture(page_,output/L"ui_combo.png");Library(1,0);Capture(page_,output/L"ui_weapons.png");Library(0,0);SearchAt({520,260});Capture(page_,output/L"ui_search.png");
    RECT after{};GetClipCursor(&after);const bool hidden=!IsWindowVisible(host_)&&!IsWindowVisible(page_)&&GetCapture()!=page_;
    manifest<<"],\"count\":"<<count<<",\"hidden\":"<<(hidden?"true":"false")<<",\"foreground_unchanged\":"<<(foreground==GetForegroundWindow()?"true":"false")<<"}";manifest.close();
    ComboMenu::Shutdown();DestroyWindow(host_);WeaponIcons::Shutdown();UiTheme::Shutdown();
    std::printf("nodes=%d hidden=%d cursor_clip_unchanged=%d\n",count,hidden,EqualRect(&clip,&after));
    return hidden&&EqualRect(&clip,&after)?0:1;
}
