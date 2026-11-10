#pragma once
#include "bone_motion_model.h"
#include <windows.h>
#include <functional>
#include <string>
namespace BoneMotionPage {
HWND CreatePage(HWND parent);
void Tick();
void Shutdown();
struct Host {
    std::function<bool(BoneMotion::Config&,std::wstring&)> read;
    std::function<void(const BoneMotion::Config&)> write;
};
void Open(HWND page,int node,Host host);
void Close();
int EditedNode();
void TickEditor(HWND page);
}
