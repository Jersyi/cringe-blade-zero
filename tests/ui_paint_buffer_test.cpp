#include "ui_paint_buffer.h"
#include <cstdio>
int main(){
    int checks=0,failures=0;const auto check=[&](bool ok){++checks;if(!ok)++failures;};
    WNDCLASSW cls{};cls.lpfnWndProc=DefWindowProcW;cls.hInstance=GetModuleHandleW(nullptr);cls.lpszClassName=L"PBZHiddenBufferTest";RegisterClassW(&cls);
    const DWORD before=GetGuiResources(GetCurrentProcess(),GR_GDIOBJECTS);
    HWND window=CreateWindowExW(WS_EX_NOACTIVATE,cls.lpszClassName,L"",WS_POPUP,0,0,64,64,nullptr,nullptr,cls.hInstance,nullptr);
    HDC screen=GetDC(nullptr),target=CreateCompatibleDC(screen);HBITMAP bitmap=CreateCompatibleBitmap(screen,64,64);ReleaseDC(nullptr,screen);auto old=SelectObject(target,bitmap);
    auto owner=UiPaintBuffer::Get(window);RECT all{0,0,64,64},dirty{10,10,20,20};
    for(int i=0;i<1000;++i){UiPaintBuffer::Frame frame(window,target,64,64);check(frame.dc()!=target);FillRect(frame.dc(),&all,static_cast<HBRUSH>(GetStockObject(WHITE_BRUSH)));}
    check(owner->allocations==1);check(GetPixel(target,0,0)==RGB(255,255,255));
    {UiPaintBuffer::Frame frame(window,target,64,64,&dirty);FillRect(frame.dc(),&all,static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));}
    check(GetPixel(target,15,15)==RGB(0,0,0));check(GetPixel(target,0,0)==RGB(255,255,255));
    for(int i=0;i<100;++i){UiPaintBuffer::Frame frame(window,target,i%2?64:32,64);FillRect(frame.dc(),&all,static_cast<HBRUSH>(GetStockObject(WHITE_BRUSH)));}
    check(owner->allocations==101);
    {UiPaintBuffer::Frame outer(window,target,64,64);UiPaintBuffer::Frame nested(window,target,64,64);check(nested.dc()==target);check(outer.dc()!=target);}
    {UiPaintBuffer::Frame huge(window,target,20000,20000);check(huge.dc()==target);check(owner->width==64);}
    {UiPaintBuffer::Frame frame(window,target,64,64);DestroyWindow(window);check(owner->busy);FillRect(frame.dc(),&all,static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));}
    check(!owner->busy);owner.reset();SelectObject(target,old);DeleteObject(bitmap);DeleteDC(target);
    check(GetGuiResources(GetCurrentProcess(),GR_GDIOBJECTS)==before);
    DWORD foreground=0;GetWindowThreadProcessId(GetForegroundWindow(),&foreground);check(foreground!=GetCurrentProcessId());
    std::printf("{\"checks\":%d,\"failures\":%d,\"hidden\":true,\"paints\":1100}\n",checks,failures);return failures?1:0;
}
