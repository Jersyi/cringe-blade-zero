#pragma once
#include <windows.h>
#include <commctrl.h>
#include <memory>
#include <new>
#include <cstdint>

// UI-thread only, one reusable bitmap per HWND. A lease survives WM_NCDESTROY;
// nested painting falls back to the supplied DC instead of corrupting the outer frame.
namespace UiPaintBuffer {
inline constexpr wchar_t kProperty[]=L"PBZ.PaintBuffer.v1";
struct Surface {
    HDC dc=nullptr;HBITMAP bitmap=nullptr;HGDIOBJ old=nullptr;int width=0,height=0;bool busy=false;
    unsigned allocations=0;
    ~Surface(){Reset();}
    void Reset(){if(dc&&old)SelectObject(dc,old);if(bitmap)DeleteObject(bitmap);if(dc)DeleteDC(dc);dc=nullptr;bitmap=nullptr;old=nullptr;width=height=0;}
    bool Ensure(HDC target,int w,int h){
        if(w<=0||h<=0||w>16384||h>16384||static_cast<uint64_t>(w)*h>16777216)return false;
        if(dc&&width==w&&height==h)return true;
        HDC next=CreateCompatibleDC(target);if(!next)return false;
        HBITMAP image=CreateCompatibleBitmap(target,w,h);if(!image){DeleteDC(next);return false;}
        HGDIOBJ previous=SelectObject(next,image);if(!previous||previous==HGDI_ERROR){DeleteObject(image);DeleteDC(next);return false;}
        Reset();dc=next;bitmap=image;old=previous;width=w;height=h;++allocations;return true;
    }
};
using Owner=std::shared_ptr<Surface>;
inline LRESULT CALLBACK Cleanup(HWND window,UINT message,WPARAM wp,LPARAM lp,UINT_PTR id,DWORD_PTR data){
    if(message==WM_NCDESTROY){RemovePropW(window,kProperty);RemoveWindowSubclass(window,Cleanup,id);delete reinterpret_cast<Owner*>(data);}
    return DefSubclassProc(window,message,wp,lp);
}
inline Owner Get(HWND window){
    if(auto* owner=reinterpret_cast<Owner*>(GetPropW(window,kProperty)))return *owner;
    if(!IsWindow(window))return {};
    Owner* owner=nullptr;try{owner=new Owner(std::make_shared<Surface>());}catch(const std::bad_alloc&){return {};}
    if(!SetPropW(window,kProperty,owner)){delete owner;return {};}
    if(!SetWindowSubclass(window,Cleanup,reinterpret_cast<UINT_PTR>(&Cleanup),reinterpret_cast<DWORD_PTR>(owner))){RemovePropW(window,kProperty);delete owner;return {};}
    return *owner;
}
class Frame {
    Owner surface_;HDC target_=nullptr,dc_=nullptr;int state_=0;RECT area_{};bool buffered_=false;
public:
    Frame(HWND window,HDC target,int w,int h,const RECT* dirty=nullptr):surface_(Get(window)),target_(target),dc_(target),area_{0,0,w,h}{
        if(dirty)IntersectRect(&area_,&area_,dirty);
        if(target&&surface_&&!surface_->busy&&surface_->Ensure(target,w,h)){surface_->busy=true;buffered_=true;dc_=surface_->dc;}
        if(dc_)state_=SaveDC(dc_);
        if(dc_)IntersectClipRect(dc_,area_.left,area_.top,area_.right,area_.bottom);
    }
    ~Frame(){
        if(state_)RestoreDC(dc_,state_);
        if(buffered_){if(target_&&!IsRectEmpty(&area_))BitBlt(target_,area_.left,area_.top,area_.right-area_.left,area_.bottom-area_.top,dc_,area_.left,area_.top,SRCCOPY);surface_->busy=false;}
    }
    Frame(const Frame&)=delete;Frame& operator=(const Frame&)=delete;
    HDC dc()const{return dc_;}
};
}
