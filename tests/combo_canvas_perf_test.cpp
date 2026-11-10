#define wmain UnusedInteractivePreviewEntry
#include "combo_menu_preview.cpp"
#undef wmain
#include "settings_store.h"
#include "combo_file.h"
#include "weapon_icons.h"
#include <filesystem>
#include <chrono>

int wmain(){
    namespace fs=std::filesystem;
    using namespace ComboMenu::Probe;
    SetErrorMode(SEM_FAILCRITICALERRORS|SEM_NOGPFAULTERRORBOX);
    SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_UNAWARE);
    const auto root=fs::temp_directory_path()/(L"pbz_canvas_perf_"+std::to_wstring(GetCurrentProcessId())+L"_"+std::to_wstring(GetTickCount64()));
    fs::create_directory(root);SettingsStore::Initialize((root/L"test.ini").c_str());SettingsStore::Put(L"NpcCombo",L"File",L"perf_test");
    const auto packed=ComboFile::Pack({});FILE* seed=nullptr;_wfopen_s(&seed,(root/L"perf_test.combo").c_str(),L"wb");if(!seed)return 2;fwrite(packed.data(),1,packed.size(),seed);fclose(seed);
    NpcCombo::SetFolder(root.wstring());UiCursor::SetHandsOff(true);UiTheme::Initialize();
    WNDCLASSW cls{};cls.lpfnWndProc=DefWindowProcW;cls.hInstance=GetModuleHandleW(nullptr);cls.lpszClassName=L"ComboPerfHiddenHost";RegisterClassW(&cls);
    host_=CreateWindowExW(WS_EX_NOACTIVATE|WS_EX_TOOLWINDOW,cls.lpszClassName,L"",WS_POPUP,0,0,980,720,nullptr,nullptr,cls.hInstance,nullptr);
    page_=ComboMenu::CreatePage(host_);SetWindowPos(page_,nullptr,0,0,980,720,SWP_NOZORDER|SWP_NOACTIVATE);
    const auto generation=NpcCombo::Generation();
    HDC screen=GetDC(nullptr),dc=CreateCompatibleDC(screen);HBITMAP bitmap=CreateCompatibleBitmap(screen,980,720);ReleaseDC(nullptr,screen);HGDIOBJ old=SelectObject(dc,bitmap);
    const auto paint=[&]{SendMessageW(page_,WM_PRINTCLIENT,reinterpret_cast<WPARAM>(dc),PRF_CLIENT);GdiFlush();};
    const auto timed=[&](auto fn){const auto start=std::chrono::steady_clock::now();fn();return std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count();};
    int failures=0;
    for(int count:{100,1000,10000}){
        PerformanceFixture(count);const double cold=timed(paint);const RECT box=NodeRect(0);const POINT hover{box.left+25,box.top+10};
        const DWORD gdi=GetGuiResources(GetCurrentProcess(),GR_GDIOBJECTS);
        uint64_t m0=0,g0=0,s0=0,m1=0,g1=0,s1=0;PerformanceStats(m0,g0,s0);
        const double warm=timed([&]{for(int i=0;i<10;++i)paint();})/10;
        const double hover_ms=timed([&]{for(int i=0;i<10;++i){PerformanceHover(i%2?hover:POINT{680,600});paint();}})/10;
        PerformanceStats(m1,g1,s1);if(m1!=m0||g1!=g0||s1!=s0){++failures;std::fprintf(stderr,"hover cache rebuilt at %d nodes\n",count);}
        const double pan=timed([&]{for(int i=0;i<10;++i){PerformancePan(i%2?4:-4);paint();}})/10;
        PerformanceStats(m1,g1,s1);if(m1!=m0||g1!=g0){++failures;std::fprintf(stderr,"pan text/topology rebuilt at %d nodes\n",count);}
        const DWORD after=GetGuiResources(GetCurrentProcess(),GR_GDIOBJECTS);
        std::printf("{\"nodes\":%d,\"cold_ms\":%.3f,\"warm_ms\":%.3f,\"hover_ms\":%.3f,\"pan_ms\":%.3f,\"gdi_delta\":%ld,\"hidden\":true}\n",count,cold,warm,hover_ms,pan,static_cast<long>(after)-static_cast<long>(gdi));
    }
    DWORD foreground=0;GetWindowThreadProcessId(GetForegroundWindow(),&foreground);
    const bool ok=!failures&&foreground!=GetCurrentProcessId()&&!IsWindowVisible(page_)&&generation==NpcCombo::Generation()&&!fs::exists(root/L"default.combo");
    SelectObject(dc,old);DeleteObject(bitmap);DeleteDC(dc);ComboMenu::Shutdown();DestroyWindow(host_);WeaponIcons::Shutdown();UiTheme::Shutdown();
    for(const auto& file:fs::directory_iterator(root))if(file.is_regular_file())fs::remove(file.path());fs::remove(root);
    return ok?0:1;
}
