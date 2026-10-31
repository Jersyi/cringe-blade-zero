#pragma once
#include "ui_host.h"
#include <windows.h>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>

// UI worker only. A 32-bit top-down DIB for per-pixel-alpha popups: paint the
// panel opaquely with GDI/GDI+, then Shape() derives premultiplied alpha from a
// rounded rectangle (anti-aliased edge, hairline border and soft drop shadow).
namespace UiSurface {
inline double RoundedDistance(double x,double y,const RECT& body,double radius) {
    const double cx=(body.left+body.right)/2.0,cy=(body.top+body.bottom)/2.0;
    const double hw=(body.right-body.left)/2.0,hh=(body.bottom-body.top)/2.0;
    const double qx=std::fabs(x-cx)-hw+radius,qy=std::fabs(y-cy)-hh+radius;
    const double ox=(std::max)(qx,0.0),oy=(std::max)(qy,0.0);
    return std::sqrt(ox*ox+oy*oy)+(std::min)((std::max)(qx,qy),0.0)-radius;
}
struct Surface {
    HDC dc=nullptr; HBITMAP bitmap=nullptr; HGDIOBJ old=nullptr; uint32_t* pixels=nullptr; int width=0,height=0;
    Surface()=default;
    Surface(const Surface&)=delete;
    Surface& operator=(const Surface&)=delete;
    ~Surface() { Reset(); }
    void Reset() {
        if (dc&&old) SelectObject(dc,old);
        if (bitmap) DeleteObject(bitmap);
        if (dc) DeleteDC(dc);
        dc=nullptr; bitmap=nullptr; old=nullptr; pixels=nullptr; width=height=0;
    }
    bool Ensure(int w,int h) {
        if (dc&&w==width&&h==height) return true;
        Reset(); if (w<=0||h<=0) return false;
        HDC screen=GetDC(nullptr); if (!screen) return false;
        dc=CreateCompatibleDC(screen);
        BITMAPINFO info{}; info.bmiHeader.biSize=sizeof(BITMAPINFOHEADER); info.bmiHeader.biWidth=w; info.bmiHeader.biHeight=-h;
        info.bmiHeader.biPlanes=1; info.bmiHeader.biBitCount=32; info.bmiHeader.biCompression=BI_RGB;
        void* data=nullptr; bitmap=CreateDIBSection(screen,&info,DIB_RGB_COLORS,&data,nullptr,0); ReleaseDC(nullptr,screen);
        if (!dc||!bitmap||!data) { Reset(); return false; }
        old=SelectObject(dc,bitmap); pixels=static_cast<uint32_t*>(data); width=w; height=h; return true;
    }
    void Clear() { if (pixels) std::memset(pixels,0,static_cast<size_t>(width)*height*4); }
    void Shape(const RECT& body,double radius,COLORREF border,int shadow,double shadow_alpha) {
        if (!pixels) return;
        GdiFlush();
        const double drop=shadow*.35;
        for (int y=0;y<height;++y) for (int x=0;x<width;++x) {
            uint32_t& pixel=pixels[static_cast<size_t>(y)*width+x];
            const double px=x+.5,py=y+.5,d=RoundedDistance(px,py,body,radius);
            const double cover=std::clamp(.5-d,0.0,1.0);
            double r=(pixel>>16)&255,g=(pixel>>8)&255,b=pixel&255;
            const double edge=std::clamp(1.0-std::fabs(d+.6),0.0,1.0);
            r+=(GetRValue(border)-r)*edge; g+=(GetGValue(border)-g)*edge; b+=(GetBValue(border)-b)*edge;
            double shade=0;
            if (shadow>0&&cover<1) {
                const double s=RoundedDistance(px,py-drop,body,radius);
                const double t=std::clamp(1.0-s/shadow,0.0,1.0); shade=shadow_alpha*t*t;
            }
            const double alpha=cover+shade*(1.0-cover);
            pixel=(static_cast<uint32_t>(std::lround(alpha*255))<<24)|(static_cast<uint32_t>(std::lround(r*cover))<<16)|
                (static_cast<uint32_t>(std::lround(g*cover))<<8)|static_cast<uint32_t>(std::lround(b*cover));
        }
    }
    bool Present(HWND window,POINT position,BYTE opacity) const {
        if (!dc||!window) return false;
        POINT origin{0,0}; SIZE size{width,height}; BLENDFUNCTION blend{AC_SRC_OVER,0,opacity,AC_SRC_ALPHA};
        const bool child=(GetWindowLongPtrW(window,GWL_STYLE)&WS_CHILD)!=0;
        if (child) {
            const POINT client=UiHost::ClientOrigin();
            SetWindowPos(window,nullptr,position.x-client.x,position.y-client.y,size.cx,size.cy,SWP_NOZORDER|SWP_NOACTIVATE|SWP_NOREDRAW);
        }
        HDC screen=GetDC(nullptr); if (!screen) return false;
        const bool ok=UpdateLayeredWindow(window,screen,child?nullptr:&position,&size,dc,&origin,0,&blend,ULW_ALPHA)!=FALSE;
        ReleaseDC(nullptr,screen); return ok;
    }
};
}
