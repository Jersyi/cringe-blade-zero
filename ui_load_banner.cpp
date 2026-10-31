#include "ui_load_banner.h"
#include "ui_assets.h"
#include "ui_host.h"
#include "ui_hotkeys.h"
#include "ui_motion.h"
#include "ui_i18n.h"
#include <windows.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>
#include <objidl.h>
#include <gdiplus.h>

namespace UiLoadBanner {
namespace {
constexpr wchar_t kClass[]=L"PBZLoadSuccessBanner_v1";
// The face animation (cold look to grin, 61 frames) eases in over 2.2 s after
// the slide-in and then holds; the banner fades only after a 1.3 s hold, so the
// whole animation is always seen. Time only advances while the banner is shown.
constexpr double kDuration=4300.0,kFaceStart=300.0,kFaceUp=2200.0;
constexpr int kFaceFrames=61,kFaceSize=112,kFaceColumns=8;
constexpr double kFaceX=44.0,kFaceY=52.0,kFaceR=24.0,kRingWidth=1.4;
struct Frame { double opacity=0,progress=1,slide=-12; bool finished=false; };
Frame AtTime(double milliseconds,bool reduced) {
    if (milliseconds>=kDuration) return {0,0,reduced?0.0:-8.0,true};
    const double enter=reduced?90.0:320.0,exit=reduced?120.0:450.0;
    const double in=UiMotion::EaseOutCubic(milliseconds/enter);
    const double out=UiMotion::EaseInCubic((milliseconds-(kDuration-exit))/exit);
    return {in*(1.0-out),1.0-UiMotion::EaseInOutCubic((milliseconds-enter)/(kDuration-enter-exit)),
        reduced?0.0:-12.0*(1.0-in)-8.0*out,false};
}
struct Surface {
    HDC dc=nullptr; HBITMAP bitmap=nullptr; HGDIOBJ old=nullptr; uint32_t* pixels=nullptr;
    int width=0,height=0;
    void Reset() {
        if (dc&&old) SelectObject(dc,old);
        if (bitmap) DeleteObject(bitmap);
        if (dc) DeleteDC(dc);
        dc=nullptr; bitmap=nullptr; old=nullptr; pixels=nullptr; width=height=0;
    }
    bool Create(int w,int h) {
        Reset(); HDC screen=GetDC(nullptr); if (!screen) return false;
        dc=CreateCompatibleDC(screen); BITMAPINFO info{}; info.bmiHeader.biSize=sizeof(BITMAPINFOHEADER);
        info.bmiHeader.biWidth=w; info.bmiHeader.biHeight=-h; info.bmiHeader.biPlanes=1;
        info.bmiHeader.biBitCount=32; info.bmiHeader.biCompression=BI_RGB;
        void* data=nullptr; bitmap=CreateDIBSection(screen,&info,DIB_RGB_COLORS,&data,nullptr,0); ReleaseDC(nullptr,screen);
        if (!dc||!bitmap||!data) { Reset(); return false; }
        old=SelectObject(dc,bitmap); if (!old||old==HGDI_ERROR) { old=nullptr; Reset(); return false; }
        pixels=static_cast<uint32_t*>(data); width=w; height=h; return true;
    }
};
Surface surface_,mask_;
HWND window_=nullptr;
HFONT title_font_=nullptr,subtitle_font_=nullptr;
DWORD worker_=0;
Logger logger_=nullptr;
std::atomic<bool> active_{false};
bool shown_=false,class_ready_=false,reduced_=false;
ULONGLONG started_=0,last_frame_=0,pending_since_=0;
int x_=0,y_=0;
double scale_=1,shown_ms_=0;
ULONGLONG last_advance_=0;
std::vector<uint32_t> face_; // premultiplied BGRA sheet, kFaceColumns x 8 cells of kFaceSize
int face_stride_=0;
void Log(const char* text) { if (logger_) logger_(text); }
LRESULT CALLBACK Procedure(HWND window,UINT message,WPARAM wparam,LPARAM lparam) {
    if (message==WM_NCHITTEST) return HTTRANSPARENT;
    if (message==WM_MOUSEACTIVATE) return MA_NOACTIVATE;
    if (message==WM_ERASEBKGND) return 1;
    if (message==WM_NCDESTROY&&window_==window) { window_=nullptr; active_.store(false,std::memory_order_relaxed); }
    return DefWindowProcW(window,message,wparam,lparam);
}
void ReleaseVisuals() {
    active_.store(false,std::memory_order_relaxed);
    if (window_) { UiHost::Detach(window_); DestroyWindow(window_); }
    window_=nullptr; surface_.Reset(); mask_.Reset();
    face_.clear(); face_.shrink_to_fit(); face_stride_=0;
    if (title_font_) DeleteObject(title_font_);
    if (subtitle_font_) DeleteObject(subtitle_font_);
    title_font_=subtitle_font_=nullptr;
    if (class_ready_) UnregisterClassW(kClass,GetModuleHandleW(nullptr));
    class_ready_=false;
}
void LoadFace() {
    face_.clear(); face_stride_=0;
    Gdiplus::Bitmap* sheet=UiAssets::Bitmap(UiAssets::Id::GrinSheet);
    if (!sheet) return;
    const int width=static_cast<int>(sheet->GetWidth()),height=static_cast<int>(sheet->GetHeight());
    if (width<kFaceColumns*kFaceSize||height<((kFaceFrames+kFaceColumns-1)/kFaceColumns)*kFaceSize) return;
    Gdiplus::Rect area(0,0,width,height); Gdiplus::BitmapData data{};
    if (sheet->LockBits(&area,Gdiplus::ImageLockModeRead,PixelFormat32bppPARGB,&data)!=Gdiplus::Ok) return;
    face_.resize(static_cast<size_t>(width)*height);
    for (int y=0;y<height;++y) std::memcpy(&face_[static_cast<size_t>(y)*width],static_cast<const unsigned char*>(data.Scan0)+static_cast<ptrdiff_t>(y)*data.Stride,static_cast<size_t>(width)*4);
    sheet->UnlockBits(&data); face_stride_=width;
}
// Bilinear sample of one face frame at cell coordinates (u, v) in [0, kFaceSize).
void SampleFace(int frame,double u,double v,double rgb[3]) {
    u=std::clamp(u,0.0,kFaceSize-1.001); v=std::clamp(v,0.0,kFaceSize-1.001);
    const int x0=static_cast<int>(u),y0=static_cast<int>(v); const double fx=u-x0,fy=v-y0;
    const int ox=(frame%kFaceColumns)*kFaceSize,oy=(frame/kFaceColumns)*kFaceSize;
    auto at=[&](int x,int y){ return face_[static_cast<size_t>(oy+y)*face_stride_+ox+x]; };
    const uint32_t p[4]={at(x0,y0),at(x0+1,y0),at(x0,y0+1),at(x0+1,y0+1)};
    const double w[4]={(1-fx)*(1-fy),fx*(1-fy),(1-fx)*fy,fx*fy};
    for (int c=0;c<3;++c) { const int shift=16-8*c; double sum=0; for (int k=0;k<4;++k) sum+=w[k]*((p[k]>>shift)&255); rgb[c]=sum; }
}
bool Prepare() {
    UiHost::Tick();
    const HWND game=UiHost::Game();
    const SIZE client=UiHost::ClientSize();
    if (!game||!UiHost::InGame()||client.cx<160||client.cy<80) return false;
    const UINT dpi=GetDpiForWindow(game);
    const double requested=(std::clamp)(static_cast<double>(dpi?dpi:96)/96.0,.75,2.5);
    const int available=(std::max)(120,static_cast<int>(client.cx)-24);
    const int width=(std::min)(available,static_cast<int>(std::lround(520*requested)));
    scale_=static_cast<double>(width)/520;
    const int height=(std::max)(30,static_cast<int>(std::lround(124*scale_)));
    x_=(client.cx-width)/2; y_=static_cast<int>(std::lround(48*scale_));
    if (y_+height>client.cy) y_=(std::max)(0,(static_cast<int>(client.cy)-height)/8);
    WNDCLASSW cls{}; cls.lpfnWndProc=Procedure; cls.hInstance=GetModuleHandleW(nullptr); cls.lpszClassName=kClass;
    if (!RegisterClassW(&cls)&&GetLastError()!=ERROR_CLASS_ALREADY_EXISTS) return false;
    class_ready_=true;
    window_=CreateWindowExW(WS_EX_LAYERED|WS_EX_NOACTIVATE|WS_EX_TRANSPARENT,kClass,L"",
        WS_POPUP,0,0,width,height,nullptr,nullptr,cls.hInstance,nullptr);
    if (!window_||!surface_.Create(width,height)||!mask_.Create(width,height)||!UiHost::Attach(window_)) { ReleaseVisuals(); return false; }
    const bool game_face=*UiAssets::TitleFace()!=0;
    title_font_=CreateFontW(-static_cast<int>(std::lround((game_face?22:19)*scale_)),0,0,0,game_face?FW_NORMAL:FW_SEMIBOLD,FALSE,FALSE,FALSE,DEFAULT_CHARSET,
        OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,ANTIALIASED_QUALITY,DEFAULT_PITCH,game_face?UiAssets::TitleFace():L"Microsoft YaHei UI");
    subtitle_font_=CreateFontW(-static_cast<int>(std::lround(14*scale_)),0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,
        OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,ANTIALIASED_QUALITY,DEFAULT_PITCH,L"STZhongsong");
    if (!title_font_||!subtitle_font_) { ReleaseVisuals(); return false; }
    LoadFace();
    return true;
}
void Blend(uint32_t& pixel,int r,int g,int b,double coverage) {
    const uint32_t alpha=static_cast<uint32_t>(std::lround(UiMotion::Unit(coverage)*255));
    const uint32_t inverse=255-alpha;
    const uint32_t old_a=pixel>>24,old_r=(pixel>>16)&255,old_g=(pixel>>8)&255,old_b=pixel&255;
    const uint32_t a=alpha+(old_a*inverse+127)/255;
    const uint32_t red=(static_cast<uint32_t>(r)*alpha+old_r*inverse+127)/255;
    const uint32_t green=(static_cast<uint32_t>(g)*alpha+old_g*inverse+127)/255;
    const uint32_t blue=(static_cast<uint32_t>(b)*alpha+old_b*inverse+127)/255;
    pixel=(a<<24)|(red<<16)|(green<<8)|blue;
}
double Rounded(double x,double y,double cx,double cy,double half_w,double half_h,double radius) {
    const double qx=std::fabs(x-cx)-half_w+radius,qy=std::fabs(y-cy)-half_h+radius;
    const double ox=(std::max)(qx,0.0),oy=(std::max)(qy,0.0);
    return std::sqrt(ox*ox+oy*oy)+(std::min)((std::max)(qx,qy),0.0)-radius;
}
double Segment(double px,double py,double ax,double ay,double bx,double by) {
    const double x=bx-ax,y=by-ay,t=UiMotion::Unit(((px-ax)*x+(py-ay)*y)/(x*x+y*y));
    const double dx=px-ax-t*x,dy=py-ay-t*y; return std::sqrt(dx*dx+dy*dy);
}
void Text(const std::wstring& text,HFONT font,int top,int bottom,int r,int g,int b) {
    std::memset(mask_.pixels,0,static_cast<size_t>(mask_.width)*mask_.height*4);
    const HGDIOBJ old=SelectObject(mask_.dc,font); SetBkMode(mask_.dc,TRANSPARENT); SetTextColor(mask_.dc,RGB(255,255,255));
    RECT rect{static_cast<LONG>(std::lround(76*scale_)),static_cast<LONG>(std::lround(top*scale_)),
        mask_.width-static_cast<LONG>(std::lround(24*scale_)),static_cast<LONG>(std::lround(bottom*scale_))};
    DrawTextW(mask_.dc,text.c_str(),-1,&rect,DT_SINGLELINE|DT_TOP|DT_NOPREFIX|DT_END_ELLIPSIS);
    SelectObject(mask_.dc,old);
    for (int y=rect.top;y<(std::min)(mask_.height,static_cast<int>(rect.bottom));++y) for (int x=rect.left;x<rect.right;++x) {
        const size_t index=static_cast<size_t>(y)*mask_.width+x; const uint32_t value=mask_.pixels[index];
        const uint32_t cover=(std::max)({value&255,(value>>8)&255,(value>>16)&255});
        if (cover) Blend(surface_.pixels[index],r,g,b,static_cast<double>(cover)/255);
    }
}
bool Draw(const Frame& frame,double milliseconds) {
    if (!window_||!surface_.pixels) return false;
    const double sweep=reduced_?-1000.0:-150.0+800*UiMotion::EaseOutExpo(milliseconds/kDuration);
    for (int y=0;y<surface_.height;++y) for (int x=0;x<surface_.width;++x) {
        const double px=(x+.5)/scale_,py=(y+.5)/scale_;
        uint32_t pixel=0; const double distance=Rounded(px,py,260,52,248,44,14);
        if (distance>0&&distance<10) { const double glow=1.0-distance/10; Blend(pixel,0,0,0,glow*glow*.42); }
        const double body=UiMotion::Unit(.65-distance);
        if (body>0) {
            const double tint=UiMotion::Unit((py-8)/88);
            // Ink body like the game's confirmation panels, pale-gold rim and accents.
            Blend(pixel,static_cast<int>(17-7*tint),static_cast<int>(16-7*tint),static_cast<int>(15-6*tint),body*.95);
            const double edge=UiMotion::Unit(1.2-std::fabs(distance)),light=UiMotion::Unit(1.0-std::fabs(px-sweep)/110);
            Blend(pixel,216,194,146,edge*(.30+.34*light));
        }
        if (!face_.empty()&&std::fabs(px-kFaceX)<kFaceR+2&&std::fabs(py-kFaceY)<kFaceR+2) {
            const double dx=px-kFaceX,dy=py-kFaceY,radius=std::sqrt(dx*dx+dy*dy);
            const double inside=UiMotion::Unit(.5+(kFaceR-radius)*scale_);
            if (inside>0) {
                // Smoothstep over the frames with a cross-fade, as in the source player.
                const double t=UiMotion::Unit((milliseconds-kFaceStart)/kFaceUp),amount=t*t*(3-2*t);
                const double f=amount*(kFaceFrames-1); const int i=static_cast<int>(f),j=(std::min)(i+1,kFaceFrames-1); const double a=f-i;
                const double u=(dx+kFaceR)/(2*kFaceR)*kFaceSize,v=(dy+kFaceR)/(2*kFaceR)*kFaceSize;
                double first[3],second[3]; SampleFace(i,u,v,first);
                if (a>0&&j!=i) { SampleFace(j,u,v,second); for (int c=0;c<3;++c) first[c]+=(second[c]-first[c])*a; }
                Blend(pixel,static_cast<int>(std::lround(first[0])),static_cast<int>(std::lround(first[1])),static_cast<int>(std::lround(first[2])),inside);
            }
            Blend(pixel,216,194,146,UiMotion::Unit(.5+(kRingWidth/2-std::fabs(radius-kFaceR))*scale_)*.95);
        } else if (face_.empty()&&std::fabs(px-44)<22&&std::fabs(py-50)<22) {
            const double dx=px-44,dy=py-50,ring=std::fabs(std::sqrt(dx*dx+dy*dy)-17);
            Blend(pixel,216,194,146,UiMotion::Unit(1.1-ring)*.8);
            const double check=(std::min)(Segment(px,py,37,50,42,55),Segment(px,py,42,55,53,43));
            Blend(pixel,236,224,196,UiMotion::Unit(1.8-check));
        }
        const double track=Rounded(px,py,260,109,232,2,2); Blend(pixel,74,70,64,UiMotion::Unit(.6-track)*.45);
        const double extent=464*frame.progress;
        if (extent>0) {
            const double fill=Rounded(px,py,28+extent/2,109,extent/2,2,(std::min)(2.0,extent/2));
            Blend(pixel,216,194,146,UiMotion::Unit(.6-fill)*.94);
        }
        surface_.pixels[static_cast<size_t>(y)*surface_.width+x]=pixel;
    }
    Text(UiI18n::Translate(L"豪之刃 已加载"),title_font_,25,51,247,242,231);
    const std::wstring subtitle=UiHotkeys::Plain(UiHotkeys::Get(UiHotkeys::Action::ToggleHub))+L"  "+UiI18n::Translate(L"打开豪之刃");
    Text(subtitle,subtitle_font_,55,76,160,153,141);
    const int top=(std::max)(0,y_+static_cast<int>(std::lround(frame.slide*scale_)));
    if ((GetWindowLongPtrW(window_,GWL_STYLE)&WS_CHILD)==0) return false;
    SetWindowPos(window_,HWND_TOP,x_,top,surface_.width,surface_.height,SWP_NOACTIVATE|SWP_SHOWWINDOW);
    POINT origin{0,0}; SIZE size{surface_.width,surface_.height};
    BLENDFUNCTION blend{AC_SRC_OVER,0,static_cast<BYTE>(std::lround(UiMotion::Unit(frame.opacity)*255)),AC_SRC_ALPHA};
    HDC screen=GetDC(nullptr); if (!screen) return false;
    const bool result=UpdateLayeredWindow(window_,screen,nullptr,&size,surface_.dc,&origin,0,&blend,ULW_ALPHA)!=FALSE;
    ReleaseDC(nullptr,screen); return result;
}
void Advance(ULONGLONG now) {
    if (!active_.load(std::memory_order_relaxed)) return;
    UiHost::Tick();
    const bool in_game=UiHost::InGame();
    // Count only on-screen time (capped per step), so alt-tab or a hitch never
    // skips part of the face animation.
    const ULONGLONG step=last_advance_?now-last_advance_:0; last_advance_=now;
    if (in_game&&window_&&IsWindowVisible(window_)) shown_ms_+=static_cast<double>((std::min)(step,ULONGLONG{50}));
    const double elapsed=shown_ms_; const Frame frame=AtTime(elapsed,reduced_);
    if (frame.finished) { ReleaseVisuals(); Log("LOAD_BANNER completed; visuals released, no timer retained"); return; }
    if (!in_game) { if (window_) ShowWindow(window_,SW_HIDE); return; }
    if (window_&&!IsWindowVisible(window_)) ShowWindow(window_,SW_SHOWNA);
    if (last_frame_&&now-last_frame_<15) return;
    last_frame_=now;
    if (!Draw(frame,elapsed)) { ReleaseVisuals(); Log("LOAD_BANNER render failed; passive notification retired"); }
}
}
void Initialize(Logger logger) {
    if (worker_) return;
    worker_=GetCurrentThreadId(); logger_=logger; shown_=false;
}
void ShowSuccess() {
    if (!worker_||worker_!=GetCurrentThreadId()||shown_||pending_since_) return;
    pending_since_=GetTickCount64(); reduced_=UiMotion::ReducedMotion();
}
void Tick() {
    if (!worker_||worker_!=GetCurrentThreadId()) return;
    const ULONGLONG now=GetTickCount64();
    if (pending_since_&&!window_) {
        if (Prepare()) {
            pending_since_=0; shown_=true; started_=now; last_frame_=0; shown_ms_=0; last_advance_=now;
            active_.store(true,std::memory_order_relaxed);
            if (!Draw(AtTime(0,reduced_),0)) { ReleaseVisuals(); Log("LOAD_BANNER render failed; passive notification retired"); return; }
            Log(face_.empty()?"LOAD_BANNER success shown once; passive/click-through, 4300ms on-screen timeline (face frames unavailable, check mark)":
                "LOAD_BANNER success shown once; passive/click-through, 4300ms on-screen timeline, 61-frame face animation");
        } else if (now-pending_since_>15000) {
            pending_since_=0; Log("LOAD_BANNER could not attach inside the game");
        }
    }
    Advance(now);
}
bool Active() { return active_.load(std::memory_order_relaxed); }
void Shutdown() {
    if (worker_&&worker_!=GetCurrentThreadId()) return;
    ReleaseVisuals(); worker_=0; logger_=nullptr; shown_=false; pending_since_=0; started_=last_frame_=0;
}
}
