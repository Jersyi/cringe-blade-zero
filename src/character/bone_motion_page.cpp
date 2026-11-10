#include "ui_paint_buffer.h"
#include "bone_motion_page.h"
#include "bone_motion.h"
#include "bone_debug_projection.h"
#include "bone_body_editor.h"
#include "bone_slider.h"
#include "debug_hub.h"
#include "ui_theme.h"
#include "ui_layout.h"
#include "ui_i18n.h"
#include <commctrl.h>
#include <memory>
#include <cwchar>
#include <windowsx.h>

namespace BoneMotionPage {
namespace {
constexpr wchar_t kClass[]=L"PBZBoneMotionPage";
enum {On=801,Feet,List,Add,Remove,Preset,Apply,Bone,RowOn,Motion,Search,DebugOn,Front,Side,Back,View,
    Simple,Detail,Group,GroupApply,Edited,Children,Mirror,Reset,ResetAll,Supplement,DebugTab,Number=830,GroupNumber=850,GroupSlider=860,RowSlider=880,Section=900,SectionReset=910,PresetsTab=920,StatusTab,Precise,UniformSlider=894,UniformNumber=844,GroupUniformSlider=895,GroupUniformNumber=845};
struct Editor {
    HWND window=nullptr,owner=nullptr,title=nullptr,intro=nullptr,on=nullptr,feet=nullptr,list=nullptr,add=nullptr,remove=nullptr,preset=nullptr,apply=nullptr,bone=nullptr,row_on=nullptr,motion=nullptr,status=nullptr;
    std::vector<HWND> labels,fields;BoneMotion::Config config;Host host;int node=-1,selected=-1,scroll=0,total=0;bool filling=false;std::string mesh;ULONGLONG next=0,status_next=0;
    HWND search=nullptr,debug_on=nullptr,front=nullptr,side=nullptr,back=nullptr,view=nullptr;bool debug=false;double yaw=0;
    std::vector<std::string> catalog;BoneMotionRuntime::Snapshot pose;std::vector<BoneDebug::Point> projected;
    HWND simple=nullptr,detail=nullptr,group=nullptr,group_apply=nullptr,group_info=nullptr,edited=nullptr,children=nullptr,mirror=nullptr,reset=nullptr,reset_all=nullptr,donor=nullptr,supplement=nullptr;
    HWND presets_tab=nullptr,status_tab=nullptr,precise=nullptr,character=nullptr,uniform_label=nullptr,uniform_field=nullptr,uniform_slider=nullptr;bool precision=false,ready=false,laying_out=false;std::array<HWND,4> section_reset{};
    HWND debug_tab=nullptr;std::array<HWND,4> sections{};std::array<bool,4> expanded{false,false,true,false};std::vector<HWND> row_sliders;
    std::array<HWND,3> group_fields{},sliders{},group_labels{};std::vector<int> row_map;int panel=1;
    HWND group_uniform_label=nullptr,group_uniform_field=nullptr,group_uniform_slider=nullptr;
};
std::unique_ptr<Editor> page_,popup_;
std::wstring Wide(const std::string& s){const int n=MultiByteToWideChar(CP_UTF8,0,s.data(),static_cast<int>(s.size()),nullptr,0);std::wstring w(static_cast<size_t>(n),L' ');if(n)MultiByteToWideChar(CP_UTF8,0,s.data(),static_cast<int>(s.size()),w.data(),n);return w;}
std::string Utf8(const std::wstring& w){const int n=WideCharToMultiByte(CP_UTF8,0,w.data(),static_cast<int>(w.size()),nullptr,0,nullptr,nullptr);std::string s(static_cast<size_t>(n),' ');if(n)WideCharToMultiByte(CP_UTF8,0,w.data(),static_cast<int>(w.size()),s.data(),n,nullptr,nullptr);return s;}
std::string Lower(std::string s){for(char& c:s)c=static_cast<char>(std::tolower(static_cast<unsigned char>(c)));return s;}
void SelectBone(Editor& e,const std::string& name){const auto w=Wide(name);const LRESULT i=SendMessageW(e.bone,CB_FINDSTRINGEXACT,static_cast<WPARAM>(-1),reinterpret_cast<LPARAM>(w.c_str()));SendMessageW(e.bone,CB_SETCURSEL,i,0);}
void BoneList(Editor& e){const std::string keep=e.selected>=0&&static_cast<size_t>(e.selected)<e.config.rows.size()?e.config.rows[static_cast<size_t>(e.selected)].bone:Utf8(UiLayout::Caption(e.bone));
    const std::string query=Lower(Utf8(UiLayout::Caption(e.search)));SendMessageW(e.bone,CB_RESETCONTENT,0,0);
    for(const auto& bone:e.catalog)if(!BoneMotion::Facial(bone)&&Lower(bone).find(query)!=std::string::npos){const auto w=Wide(bone);SendMessageW(e.bone,CB_ADDSTRING,0,reinterpret_cast<LPARAM>(w.c_str()));}
    if(query.empty()&&!keep.empty()&&!BoneMotion::Facial(keep)&&std::find(e.catalog.begin(),e.catalog.end(),keep)==e.catalog.end()){const auto w=Wide(keep);SendMessageW(e.bone,CB_ADDSTRING,0,reinterpret_cast<LPARAM>(w.c_str()));}
    SelectBone(e,keep);
}
void DebugDemand(){BoneMotionRuntime::Debug((page_&&page_->debug&&IsWindowVisible(page_->window))||(popup_&&popup_->debug&&IsWindowVisible(popup_->window)));}
void PaintView(Editor& e,HDC dc){RECT rc{};GetClientRect(e.view,&rc);FillRect(dc,&rc,UiTheme::BaseBrush());SetBkMode(dc,TRANSPARENT);
    const HGDIOBJ oldFont=SelectObject(dc,UiTheme::GetFont(UiTheme::Font::Small,e.view));
    e.projected=BoneDebug::Project(e.pose.bones,e.pose.positions,e.yaw,rc.right,rc.bottom,28);
    HPEN pen=CreatePen(PS_SOLID,1,UiTheme::Accent());const HGDIOBJ oldPen=SelectObject(dc,pen);const std::string chosen=Utf8(UiLayout::Caption(e.bone));int selected=-1;
    for(size_t i=0;i<e.projected.size();++i){const auto& p=e.projected[i];if(!p.shown)continue;if(e.pose.bones[i]==chosen)selected=static_cast<int>(i);
        int parent=i<e.pose.parents.size()?e.pose.parents[i]:-1;for(size_t step=0;step<e.projected.size()&&parent>=0&&static_cast<size_t>(parent)<e.projected.size()&&!e.projected[static_cast<size_t>(parent)].shown;++step)parent=static_cast<size_t>(parent)<e.pose.parents.size()?e.pose.parents[static_cast<size_t>(parent)]:-1;
        if(parent>=0&&static_cast<size_t>(parent)<e.projected.size()){const auto& q=e.projected[static_cast<size_t>(parent)];MoveToEx(dc,static_cast<int>(q.x),static_cast<int>(q.y),nullptr);LineTo(dc,static_cast<int>(p.x),static_cast<int>(p.y));}
        SetPixelV(dc,static_cast<int>(p.x),static_cast<int>(p.y),UiTheme::Text());}
    SelectObject(dc,oldPen);DeleteObject(pen);SetTextColor(dc,UiTheme::MutedText());
    std::wstring label=e.pose.positions.empty()?L"等待角色骨架…":L"完整骨架 · 点击选骨骼 · 滚轮转向 · 已排除表情骨骼";
    RECT text{8,5,rc.right-8,25};DrawTextW(dc,label.c_str(),-1,&text,DT_LEFT|DT_SINGLELINE|DT_END_ELLIPSIS|DT_NOPREFIX);
    if(selected>=0){const auto& p=e.projected[static_cast<size_t>(selected)];HBRUSH brush=CreateSolidBrush(UiTheme::Accent());const HGDIOBJ oldBrush=SelectObject(dc,brush);Ellipse(dc,static_cast<int>(p.x)-4,static_cast<int>(p.y)-4,static_cast<int>(p.x)+5,static_cast<int>(p.y)+5);SelectObject(dc,oldBrush);DeleteObject(brush);
        const auto& pos=e.pose.positions[static_cast<size_t>(selected)];wchar_t detail[220]{};swprintf_s(detail,L"%ls · CS %.2f / %.2f / %.2f cm",Wide(chosen).c_str(),pos[0],pos[1],pos[2]);text={8,rc.bottom-24,rc.right-8,rc.bottom-3};SetTextColor(dc,UiTheme::Text());DrawTextW(dc,detail,-1,&text,DT_LEFT|DT_SINGLELINE|DT_END_ELLIPSIS|DT_NOPREFIX);}
    SelectObject(dc,oldFont);
}
LRESULT CALLBACK ViewProc(HWND w,UINT msg,WPARAM wp,LPARAM lp,UINT_PTR,DWORD_PTR data){auto& e=*reinterpret_cast<Editor*>(data);
    if(msg==WM_PAINT||msg==WM_PRINTCLIENT){PAINTSTRUCT ps{};HDC target=msg==WM_PAINT?BeginPaint(w,&ps):reinterpret_cast<HDC>(wp);RECT rc{};GetClientRect(w,&rc);{UiPaintBuffer::Frame buffer(w,target,(std::max)(1L,rc.right),(std::max)(1L,rc.bottom),msg==WM_PAINT?&ps.rcPaint:nullptr);if(buffer.dc())PaintView(e,buffer.dc());}if(msg==WM_PAINT)EndPaint(w,&ps);return 0;}
    if(msg==WM_ERASEBKGND)return 1;
    if(msg==WM_MOUSEWHEEL){e.yaw+=static_cast<short>(HIWORD(wp))/120.0*0.15;InvalidateRect(w,nullptr,FALSE);return 0;}
    if(msg==WM_LBUTTONUP){const int x=GET_X_LPARAM(lp),y=GET_Y_LPARAM(lp);double best=100;int index=-1;for(size_t i=0;i<e.projected.size();++i){const auto& p=e.projected[i];const double d=(p.x-x)*(p.x-x)+(p.y-y)*(p.y-y);if(p.shown&&d<best){best=d;index=static_cast<int>(i);}}
        if(index>=0){SetWindowTextW(e.search,L"");SelectBone(e,e.pose.bones[static_cast<size_t>(index)]);InvalidateRect(w,nullptr,FALSE);}return 0;}
    return DefSubclassProc(w,msg,wp,lp);
}
LRESULT CALLBACK EditPrintProc(HWND w,UINT msg,WPARAM wp,LPARAM lp,UINT_PTR,DWORD_PTR){
    if(msg==WM_PRINT||msg==WM_PRINTCLIENT){HDC dc=reinterpret_cast<HDC>(wp);RECT r{};GetClientRect(w,&r);FillRect(dc,&r,UiTheme::PanelBrush());HBRUSH border=CreateSolidBrush(UiTheme::Border());FrameRect(dc,&r,border);DeleteObject(border);InflateRect(&r,-7,0);SetBkMode(dc,TRANSPARENT);SetTextColor(dc,IsWindowEnabled(w)?UiTheme::Text():UiTheme::MutedText());auto old=SelectObject(dc,UiTheme::GetFont(UiTheme::Font::Body,w));auto text=UiLayout::Caption(w);
        if(text.empty()){wchar_t cue[180]{};SendMessageW(w,EM_GETCUEBANNER,reinterpret_cast<WPARAM>(cue),180);text=cue;SetTextColor(dc,UiTheme::MutedText());}DrawTextW(dc,text.c_str(),-1,&r,DT_SINGLELINE|DT_VCENTER|DT_END_ELLIPSIS|DT_NOPREFIX);SelectObject(dc,old);return 0;}
    return DefSubclassProc(w,msg,wp,lp);
}
HWND Control(Editor& e,const wchar_t* cls,const wchar_t* text,DWORD style,int id=0){HWND w=CreateWindowExW(!_wcsicmp(cls,L"EDIT")?WS_EX_CLIENTEDGE:0,cls,text,WS_CHILD|WS_VISIBLE|style,0,0,10,10,e.window,reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),GetModuleHandleW(nullptr),nullptr);
    SendMessageW(w,WM_SETFONT,reinterpret_cast<WPARAM>(UiTheme::GetFont(UiTheme::Font::Body,e.window)),FALSE);if(!_wcsicmp(cls,L"EDIT"))SetWindowSubclass(w,EditPrintProc,99,0);return w;}
void Layout(Editor& e){if(!e.ready||e.laying_out)return;e.laying_out=true;struct Exit{bool& value;~Exit(){value=false;}} exit{e.laying_out};const auto m=UiLayout::For(e.window);RECT rc{};GetClientRect(e.window,&rc);const int x=m.S(12),gap=m.S(5),w=(std::max)(m.S(380),static_cast<int>(rc.right)-2*x),row=m.S(28);int y=x-e.scroll;
    const auto visibility=[](HWND h,int cmd){if(h&&((GetWindowLongPtrW(h,GWL_STYLE)&WS_VISIBLE)!=0)!=(cmd!=SW_HIDE))ShowWindow(h,cmd);};
    const auto place=[&](HWND h,int xx,int yy,int ww,int hh){UiLayout::Place(h,xx,yy,ww,hh);};
    place(e.title,x,y,w-m.S(110),row);place(e.on,x+w-m.S(100),y,m.S(100),row);y+=row+gap;
    const std::wstring info=L"角色："+(e.mesh.empty()?L"等待角色":Wide(e.mesh))+L"  ·  "+std::to_wstring(e.catalog.size())+L" 骨骼  ·  "+std::to_wstring(e.config.rows.size())+L" 项调整";
    if(UiLayout::Caption(e.character)!=info)SetWindowTextW(e.character,info.c_str());place(e.character,x,y,w,m.S(22));y+=m.S(22)+gap;
    int tabx=x;for(const auto& item:std::array<std::pair<HWND,int>,5>{{{e.simple,0},{e.detail,1},{e.presets_tab,3},{e.status_tab,4},{e.debug_tab,2}}}){const int tw=m.S(item.second==2?90:65);place(item.first,tabx,y,tw,row);tabx+=tw+3;BoneEditorSkin::Selected(item.first,e.panel==item.second);}y+=row+gap;
    for(HWND h:{e.list,e.add,e.remove,e.search,e.bone,e.row_on,e.motion,e.apply,e.edited,e.children,e.mirror,e.reset})visibility(h,e.panel==1?SW_SHOWNA:SW_HIDE);
    for(size_t i=0;i<e.labels.size();++i){const bool show=e.panel==1&&e.expanded[i<9?i/3:3];visibility(e.labels[i],show?SW_SHOWNA:SW_HIDE);visibility(e.fields[i],show&&e.precision?SW_SHOWNA:SW_HIDE);visibility(e.row_sliders[i],show?SW_SHOWNA:SW_HIDE);}
    for(HWND h:e.sections)visibility(h,e.panel==1?SW_SHOWNA:SW_HIDE);for(HWND h:e.section_reset)visibility(h,e.panel==1?SW_SHOWNA:SW_HIDE);
    for(HWND h:{e.uniform_label,e.uniform_slider})visibility(h,e.panel==1&&e.expanded[2]?SW_SHOWNA:SW_HIDE);visibility(e.uniform_field,e.panel==1&&e.expanded[2]&&e.precision?SW_SHOWNA:SW_HIDE);
    for(HWND h:{e.group,e.group_info,e.group_apply})visibility(h,e.panel==0?SW_SHOWNA:SW_HIDE);
    for(HWND h:e.group_labels)visibility(h,e.panel==0?SW_SHOWNA:SW_HIDE);for(HWND h:e.group_fields)visibility(h,e.panel==0&&e.precision?SW_SHOWNA:SW_HIDE);for(HWND h:e.sliders)visibility(h,e.panel==0?SW_SHOWNA:SW_HIDE);
    for(HWND h:{e.group_uniform_label,e.group_uniform_slider})visibility(h,e.panel==0?SW_SHOWNA:SW_HIDE);visibility(e.group_uniform_field,e.panel==0&&e.precision?SW_SHOWNA:SW_HIDE);
    for(HWND h:{e.debug_on,e.front,e.side,e.back,e.view})visibility(h,e.panel==2?SW_SHOWNA:SW_HIDE);
    for(HWND h:{e.preset,e.reset_all})visibility(h,e.panel==3?SW_SHOWNA:SW_HIDE);
    for(HWND h:{e.status,e.supplement,e.feet,e.donor})visibility(h,e.panel==4?SW_SHOWNA:SW_HIDE);
    visibility(e.intro,e.panel==1?SW_SHOWNA:SW_HIDE);visibility(e.precise,e.panel==0||e.panel==1?SW_SHOWNA:SW_HIDE);
    const auto slider_row=[&](HWND slider,HWND field,HWND label){const int labelw=m.S(132),number=e.precision?m.S(102):0;visibility(slider,SW_SHOWNA);visibility(label,SW_SHOWNA);visibility(field,e.precision?SW_SHOWNA:SW_HIDE);
        place(slider,x,y,w-labelw-number-(e.precision?2:1)*gap,row);if(e.precision)place(field,x+w-labelw-number-gap,y,number,row);place(label,x+w-labelw+gap,y,labelw-gap,row);y+=row+gap;};
    if(e.panel==0){place(e.group,x,y,w-m.S(120),m.S(260));place(e.precise,x+w-m.S(112),y,m.S(112),row);y+=row+gap;
        place(e.group_info,x,y,w,m.S(58));y+=m.S(58)+gap;slider_row(e.group_uniform_slider,e.group_uniform_field,e.group_uniform_label);for(size_t i=0;i<3;++i)slider_row(e.sliders[i],e.group_fields[i],e.group_labels[i]);place(e.group_apply,x,y,m.S(220),row);y+=row+gap;
    }else if(e.panel==1){place(e.search,x,y,w-m.S(150)-gap,row);place(e.edited,x+w-m.S(150),y,m.S(150),row);y+=row+gap;
        place(e.list,x,y,w,m.S(156));y+=m.S(156)+gap;
        int bx=x;for(HWND h:{e.add,e.remove,e.reset}){place(h,bx,y,m.S(90),row);bx+=m.S(90)+gap;}y+=row+gap;
        place(e.bone,x,y,w-m.S(224),m.S(260));place(e.row_on,x+w-m.S(218),y,m.S(100),row);place(e.precise,x+w-m.S(112),y,m.S(112),row);y+=row+gap;
        place(e.children,x,y,w,m.S(220));y+=row+gap;
        for(size_t section:{size_t{2},size_t{1},size_t{0},size_t{3}}){place(e.sections[section],x,y,w-m.S(64)-gap,row);place(e.section_reset[section],x+w-m.S(64),y,m.S(64),row);y+=row+gap;
            if(!e.expanded[section])continue;
            if(section==2)slider_row(e.uniform_slider,e.uniform_field,e.uniform_label);
            if(section==3){visibility(e.motion,SW_SHOWNA);place(e.motion,x,y,w,m.S(220));y+=row+gap;}
            for(size_t i=section*3;i<(section==3?13:section*3+3);++i)slider_row(e.row_sliders[i],e.fields[i],e.labels[i]);}
        if(!e.expanded[3])visibility(e.motion,SW_HIDE);
        place(e.apply,x,y,m.S(130),row);place(e.mirror,x+m.S(135),y,m.S(190),row);y+=row+gap;
        place(e.intro,x,y,w,m.S(22));y+=m.S(22)+gap;
    }else if(e.panel==2){place(e.debug_on,x,y,w,row);y+=row+gap;int bx=x;for(HWND h:{e.front,e.side,e.back}){place(h,bx,y,m.S(110),row);bx+=m.S(115);}y+=row+gap;place(e.view,x,y,w,m.S(390));y+=m.S(390)+gap;}
    else if(e.panel==3){place(e.preset,x,y,m.S(180),row);place(e.reset_all,x+m.S(185),y,m.S(160),row);y+=row+gap;}
    else {place(e.supplement,x,y,w,row);y+=row+gap;place(e.feet,x,y,w,row);y+=row+gap;const int sh=UiLayout::TextHeight(e.status,w,3);place(e.status,x,y,w,sh);y+=sh+gap;const int dh=UiLayout::TextHeight(e.donor,w);place(e.donor,x,y,w,dh);y+=dh+gap;}
    e.total=y+e.scroll+x;
    if(e.node>=0){const int maxscroll=(std::max)(0,e.total-static_cast<int>(rc.bottom));if(e.scroll>maxscroll){e.scroll=maxscroll;e.laying_out=false;Layout(e);return;}SCROLLINFO si{sizeof(si),SIF_RANGE|SIF_PAGE|SIF_POS,0,e.total,static_cast<UINT>(rc.bottom),e.scroll,0};SetScrollInfo(e.window,SB_VERT,&si,TRUE);}
    if(e.node<0)DebugHub::SetMinimum(e.window,m.S(404),y+x);
}
void FillRow(Editor& e){e.filling=true;const bool valid=e.selected>=0&&static_cast<size_t>(e.selected)<e.config.rows.size();
    EnableWindow(e.remove,valid);EnableWindow(e.apply,valid);EnableWindow(e.bone,valid);EnableWindow(e.row_on,valid);EnableWindow(e.motion,valid);
    EnableWindow(e.children,valid);EnableWindow(e.reset,valid);EnableWindow(e.mirror,valid);
    for(HWND f:{e.uniform_field,e.uniform_slider})EnableWindow(f,valid);for(HWND f:e.section_reset)EnableWindow(f,valid);for(HWND f:e.fields)EnableWindow(f,valid);for(HWND f:e.row_sliders){EnableWindow(f,valid);InvalidateRect(f,nullptr,FALSE);}
    if(valid){const auto& r=e.config.rows[static_cast<size_t>(e.selected)];BoneList(e);SelectBone(e,r.bone);SendMessageW(e.row_on,BM_SETCHECK,r.on?BST_CHECKED:BST_UNCHECKED,0);SendMessageW(e.motion,CB_SETCURSEL,r.motion,0);SendMessageW(e.children,CB_SETCURSEL,r.children,0);
        std::vector<double> values;for(double v:r.translation)values.push_back(v);for(double v:r.rotation)values.push_back(v);for(double v:r.scale)values.push_back(v);for(double v:{r.strength,r.limit,r.frequency,r.damping})values.push_back(v);
        for(size_t i=0;i<values.size();++i){wchar_t text[40]{};swprintf_s(text,L"%.3f",values[i]);SetWindowTextW(e.fields[i],text);}SetWindowTextW(e.uniform_field,UiLayout::Caption(e.fields[6]).c_str());InvalidateRect(e.uniform_slider,nullptr,FALSE);}
    e.filling=false;
}
void GroupValues(Editor& e){const LRESULT choice=SendMessageW(e.group,CB_GETCURSEL,0,0);const LRESULT group=choice<0?-1:SendMessageW(e.group,CB_GETITEMDATA,choice,0);
    const bool filling=e.filling;e.filling=true;
    const auto members=group<0?std::vector<std::string>{}:BoneBodyEditor::Members(e.catalog,static_cast<size_t>(group));std::array<double,3> scale{1,1,1};std::wstring text=L"匹配骨骼：";bool mixed=false;
    for(size_t i=0;i<members.size();++i){if(i)text+=L"、";text+=Wide(members[i]);const int at=BoneBodyEditor::FindRow(e.config,members[i]);const auto value=at<0?std::array<double,3>{1,1,1}:e.config.rows[static_cast<size_t>(at)].scale;if(!i)scale=value;else if(value!=scale)mixed=true;}
    if(members.empty())text=L"当前角色没有对应骨骼；不创建无效调整。";else text+=mixed?L"\r\n左右值不同；调整后统一。":L"\r\n拖动实时调整；数值输入后点击应用。";
    SetWindowTextW(e.group_info,text.c_str());EnableWindow(e.group_apply,!members.empty());
    for(size_t i=0;i<3;++i){wchar_t value[40]{};swprintf_s(value,L"%.3f",scale[i]);SetWindowTextW(e.group_fields[i],value);InvalidateRect(e.sliders[i],nullptr,FALSE);EnableWindow(e.sliders[i],!members.empty());EnableWindow(e.group_fields[i],!members.empty());}
    SetWindowTextW(e.group_uniform_field,UiLayout::Caption(e.group_fields[0]).c_str());InvalidateRect(e.group_uniform_slider,nullptr,FALSE);
    EnableWindow(e.group_uniform_field,!members.empty());EnableWindow(e.group_uniform_slider,!members.empty());e.filling=filling;
}
void Groups(Editor& e){const LRESULT choice=SendMessageW(e.group,CB_GETCURSEL,0,0),keep=choice<0?-1:SendMessageW(e.group,CB_GETITEMDATA,choice,0);SendMessageW(e.group,CB_RESETCONTENT,0,0);int select=0;
    for(size_t i=0;i<std::size(BoneBodyEditor::Groups);++i){const auto members=BoneBodyEditor::Members(e.catalog,i);if(members.empty())continue;const auto label=std::wstring(BoneBodyEditor::Groups[i].label)+L" · "+std::to_wstring(members.size())+L" 根";
        const auto row=SendMessageW(e.group,CB_ADDSTRING,0,reinterpret_cast<LPARAM>(label.c_str()));SendMessageW(e.group,CB_SETITEMDATA,row,static_cast<LPARAM>(i));if(keep==static_cast<LRESULT>(i))select=static_cast<int>(row);}
    SendMessageW(e.group,CB_SETCURSEL,select,0);GroupValues(e);
}
void Fill(Editor& e){e.filling=true;SendMessageW(e.on,BM_SETCHECK,e.config.on?BST_CHECKED:BST_UNCHECKED,0);SendMessageW(e.feet,BM_SETCHECK,e.config.protect_feet?BST_CHECKED:BST_UNCHECKED,0);
    SendMessageW(e.list,LB_RESETCONTENT,0,0);
    e.row_map.clear();int actual=0,shown=-1;const bool only=SendMessageW(e.edited,BM_GETCHECK,0,0)==BST_CHECKED;
    for(const auto& r:e.config.rows){const int index=actual++;if(only&&r.translation==std::array<double,3>{}&&r.rotation==std::array<double,3>{}&&r.scale==std::array<double,3>{1,1,1}&&r.motion==0)continue;
        if(index==e.selected)shown=static_cast<int>(e.row_map.size());e.row_map.push_back(index);const std::wstring text=(r.on?L"● ":L"○ ")+Wide(r.bone)+(r.motion?L"  ~":L"");SendMessageW(e.list,LB_ADDSTRING,0,reinterpret_cast<LPARAM>(text.c_str()));}
    if(shown<0&&!e.row_map.empty())shown=0;e.selected=shown<0?-1:e.row_map[static_cast<size_t>(shown)];SendMessageW(e.list,LB_SETCURSEL,shown,0);Groups(e);e.filling=false;FillRow(e);}
void Save(Editor& e,bool persist=true){e.config=BoneMotion::Clean(e.config);if(e.node<0)BoneMotionRuntime::SetGlobal(e.config,persist);else {if(persist&&e.host.write)e.host.write(e.config);BoneMotionRuntime::Preview(e.config);}if(persist)Fill(e);}
bool ApplyRow(Editor& e,bool persist=true){if(e.selected<0||static_cast<size_t>(e.selected)>=e.config.rows.size())return false;auto r=e.config.rows[static_cast<size_t>(e.selected)];r.bone=Utf8(UiLayout::Caption(e.bone));
    if(!BoneMotion::Name(r.bone)||BoneMotion::Facial(r.bone)){SetWindowTextW(e.status,L"请选择游戏内的英文骨骼名，不能包含存档分隔符。未应用。");return false;}
    std::vector<double> values;for(HWND f:e.fields){const std::wstring text=UiLayout::Caption(f);wchar_t* end=nullptr;const double v=wcstod(text.c_str(),&end);while(end&&iswspace(*end))++end;
        if(end==text.c_str()||!end||*end||!std::isfinite(v)){SetWindowTextW(e.status,L"请输入有效数值；当前行未应用。");return false;}values.push_back(v);}
    for(size_t k=0;k<3;++k){r.translation[k]=values[k];r.rotation[k]=values[k+3];r.scale[k]=values[k+6];}r.strength=values[9];r.limit=values[10];r.frequency=values[11];r.damping=values[12];
    r.children=static_cast<int>(SendMessageW(e.children,CB_GETCURSEL,0,0));r.motion=static_cast<int>(SendMessageW(e.motion,CB_GETCURSEL,0,0));r.on=SendMessageW(e.row_on,BM_GETCHECK,0,0)==BST_CHECKED;e.config.rows[static_cast<size_t>(e.selected)]=r;Save(e,persist);return true;
}
void ApplyGroup(Editor& e,bool persist=true){const LRESULT choice=SendMessageW(e.group,CB_GETCURSEL,0,0);if(choice<0)return;const auto group=SendMessageW(e.group,CB_GETITEMDATA,choice,0);std::array<double,3> values{};
    for(size_t i=0;i<3;++i){const auto text=UiLayout::Caption(e.group_fields[i]);wchar_t* end=nullptr;values[i]=wcstod(text.c_str(),&end);while(end&&iswspace(*end))++end;if(!end||end==text.c_str()||*end||!std::isfinite(values[i])){SetWindowTextW(e.status,L"请输入有效缩放数值。未应用。");return;}}
    if(BoneBodyEditor::SetGroup(e.config,e.catalog,static_cast<size_t>(group),values))Save(e,persist);else SetWindowTextW(e.status,L"没有匹配骨骼，或调整行已满。未应用。");
}
LRESULT CALLBACK NumberProc(HWND w,UINT msg,WPARAM wp,LPARAM lp,UINT_PTR id,DWORD_PTR){
    if(msg==WM_MOUSEWHEEL){const std::wstring text=UiLayout::Caption(w);wchar_t* end=nullptr;double value=wcstod(text.c_str(),&end);if(end!=text.c_str()&&std::isfinite(value)){
        const double step=id>=6?0.05:((GetKeyState(VK_SHIFT)&0x8000)?0.1:1.0);value+=static_cast<short>(HIWORD(wp))/120.0*step;wchar_t out[40]{};swprintf_s(out,L"%.3f",value);SetWindowTextW(w,out);}return 0;}
    return DefSubclassProc(w,msg,wp,lp);
}
void Command(Editor& e,int id,int notification){if(e.filling)return;
    if(id==GroupUniformNumber&&notification==EN_CHANGE){const auto value=UiLayout::Caption(e.group_uniform_field);for(HWND f:e.group_fields)SetWindowTextW(f,value.c_str());InvalidateRect(e.group_uniform_slider,nullptr,FALSE);return;}
    if(id==GroupUniformSlider&&(notification==BoneSlider::Preview||notification==BoneSlider::Commit)){const auto value=UiLayout::Caption(e.group_uniform_field);for(HWND f:e.group_fields)SetWindowTextW(f,value.c_str());ApplyGroup(e,notification==BoneSlider::Commit);return;}
    if(id==UniformNumber&&notification==EN_CHANGE){const auto value=UiLayout::Caption(e.uniform_field);for(size_t k=6;k<9;++k)SetWindowTextW(e.fields[k],value.c_str());if(e.uniform_slider)InvalidateRect(e.uniform_slider,nullptr,FALSE);return;}
    if(id>=GroupNumber&&id<GroupNumber+3&&notification==EN_CHANGE){if(e.sliders[static_cast<size_t>(id-GroupNumber)])InvalidateRect(e.sliders[static_cast<size_t>(id-GroupNumber)],nullptr,FALSE);return;}
    if(id==UniformSlider&&(notification==BoneSlider::Preview||notification==BoneSlider::Commit)){const auto value=UiLayout::Caption(e.uniform_field);for(size_t k=6;k<9;++k)SetWindowTextW(e.fields[k],value.c_str());ApplyRow(e,notification==BoneSlider::Commit);return;}
    if(id>=RowSlider&&id<RowSlider+13&&(notification==BoneSlider::Preview||notification==BoneSlider::Commit)){ApplyRow(e,notification==BoneSlider::Commit);return;}
    if(id>=GroupSlider&&id<GroupSlider+3&&(notification==BoneSlider::Preview||notification==BoneSlider::Commit)){ApplyGroup(e,notification==BoneSlider::Commit);return;}
    if(id>=Number&&id<Number+13&&notification==EN_CHANGE){if(static_cast<size_t>(id-Number)<e.row_sliders.size())InvalidateRect(e.row_sliders[static_cast<size_t>(id-Number)],nullptr,FALSE);return;}
    if(id==Search&&notification==EN_CHANGE){BoneList(e);return;}
    if(id==Group&&notification==CBN_SELCHANGE){GroupValues(e);return;}
    if(id==Bone&&notification==CBN_SELCHANGE){InvalidateRect(e.view,nullptr,FALSE);return;}
    if(id==List&&notification==LBN_SELCHANGE){const auto row=SendMessageW(e.list,LB_GETCURSEL,0,0);e.selected=row<0||static_cast<size_t>(row)>=e.row_map.size()?-1:e.row_map[static_cast<size_t>(row)];FillRow(e);return;}
    if(notification!=BN_CLICKED)return;
    if(id>=Section&&id<Section+4){const int at=id-Section;e.expanded[at]=!e.expanded[at];const wchar_t* names[]={L"位移 · cm",L"旋转 · 度",L"缩放 · 倍率",L"动态 · 惯性 / 胯部"};SetWindowTextW(e.sections[at],(std::wstring(e.expanded[at]?L"▾  ":L"▸  ")+names[at]).c_str());Layout(e);}
    else if(id>=SectionReset&&id<SectionReset+4&&e.selected>=0){auto& r=e.config.rows[static_cast<size_t>(e.selected)];switch(id-SectionReset){case 0:r.translation={};break;case 1:r.rotation={};break;case 2:r.scale={1,1,1};break;default:r.motion=0;r.strength=1;r.limit=1.5;r.frequency=2.5;r.damping=.8;}Save(e);}
    else if(id==Precise){e.precision=SendMessageW(e.precise,BM_GETCHECK,0,0)==BST_CHECKED;Layout(e);}
    else if(id==Simple||id==Detail||id==DebugTab||id==PresetsTab||id==StatusTab){e.panel=id==Simple?0:id==Detail?1:id==DebugTab?2:id==PresetsTab?3:4;e.scroll=0;Layout(e);}
    else if(id==GroupApply)ApplyGroup(e);
    else if(id==Edited)Fill(e);
    else if(id==ResetAll){e.config.rows.clear();Save(e);}
    else if(id==Reset&&e.selected>=0){auto& row=e.config.rows[static_cast<size_t>(e.selected)];const auto name=row.bone;row={};row.bone=name;Save(e);}
    else if(id==Mirror&&e.selected>=0){if(!e.debug||e.pose.reference.empty()){e.debug=true;SendMessageW(e.debug_on,BM_SETCHECK,BST_CHECKED,0);DebugDemand();Layout(e);SetWindowTextW(e.status,L"正在读取参考骨架；就绪后再次点击镜像。未应用。");return;}
        if(!ApplyRow(e)||e.selected<0)return;BoneMotion::Row target;const auto& source=e.config.rows[static_cast<size_t>(e.selected)];
        if(!BoneBodyEditor::Mirror(source,e.catalog,e.pose.parents,e.pose.reference,target)){SetWindowTextW(e.status,L"无左右配对，或参考轴不支持当前非均匀缩放镜像。未修改另一侧。");return;}
        auto it=std::find_if(e.config.rows.begin(),e.config.rows.end(),[&](const auto& r){return BoneBodyEditor::Lower(r.bone)==BoneBodyEditor::Lower(target.bone)&&r.motion==target.motion;});
        if(it!=e.config.rows.end())*it=target;else if(e.config.rows.size()<BoneMotion::MaxRows)e.config.rows.push_back(target);else {SetWindowTextW(e.status,L"调整行已满；另一侧未修改。");return;}Save(e);}
    else if(id==DebugOn){e.debug=SendMessageW(e.debug_on,BM_GETCHECK,0,0)==BST_CHECKED;DebugDemand();Layout(e);}
    else if(id==Front||id==Side||id==Back){e.yaw=id==Front?0:id==Side?1.5707963267948966:3.141592653589793;InvalidateRect(e.view,nullptr,FALSE);}
    else if(id==On){e.config.on=SendMessageW(e.on,BM_GETCHECK,0,0)==BST_CHECKED;Save(e);}
    else if(id==Feet){e.config.protect_feet=SendMessageW(e.feet,BM_GETCHECK,0,0)==BST_CHECKED;Save(e);}
    else if(id==Preset){e.config=BoneMotion::Feminine();e.selected=0;Save(e);}
    else if(id==Supplement){BoneMotionRuntime::Supplement(SendMessageW(e.supplement,BM_GETCHECK,0,0)==BST_CHECKED);}
    else if(id==Add&&e.config.rows.size()<BoneMotion::MaxRows){e.config.rows.push_back({});e.selected=static_cast<int>(e.config.rows.size())-1;Save(e);}
    else if(id==Remove&&e.selected>=0){e.config.rows.erase(e.config.rows.begin()+e.selected);Save(e);}
    else if(id==Apply)ApplyRow(e);
}
LRESULT CALLBACK Proc(HWND w,UINT msg,WPARAM wp,LPARAM lp){auto* e=reinterpret_cast<Editor*>(GetWindowLongPtrW(w,GWLP_USERDATA));
    if(msg==WM_NCCREATE){e=static_cast<Editor*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);e->window=w;SetWindowLongPtrW(w,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(e));}
    if(e){if(msg==WM_COMMAND&&e->ready){Command(*e,LOWORD(wp),HIWORD(wp));return 0;}if(msg==WM_SIZE&&e->title){Layout(*e);return 0;}
        if(e->node>=0&&(msg==WM_VSCROLL||msg==WM_MOUSEWHEEL)){
            int next=e->scroll;if(msg==WM_MOUSEWHEEL)next-=static_cast<short>(HIWORD(wp))/120*48;
            else {const int code=LOWORD(wp);if(code==SB_TOP)next=0;else if(code==SB_BOTTOM)next=e->total;else if(code==SB_LINEUP)next-=24;else if(code==SB_LINEDOWN)next+=24;else if(code==SB_PAGEUP)next-=180;else if(code==SB_PAGEDOWN)next+=180;else if(code==SB_THUMBTRACK){SCROLLINFO si{sizeof(si),SIF_TRACKPOS};GetScrollInfo(w,SB_VERT,&si);next=si.nTrackPos;}}
            RECT rc{};GetClientRect(w,&rc);e->scroll=std::clamp(next,0,(std::max)(0,e->total-static_cast<int>(rc.bottom)));Layout(*e);return 0;}
        if(msg==WM_CLOSE){if(e->node>=0)Close();else DebugHub::Hide();return 0;}}
    return DefWindowProcW(w,msg,wp,lp);
}
void Create(Editor& e,HWND parent,bool popup){WNDCLASSEXW cls{};cls.cbSize=sizeof(cls);cls.hInstance=GetModuleHandleW(nullptr);cls.lpfnWndProc=Proc;cls.lpszClassName=kClass;cls.hCursor=LoadCursorW(nullptr,IDC_ARROW);cls.hbrBackground=reinterpret_cast<HBRUSH>(COLOR_BTNFACE+1);RegisterClassExW(&cls);
    INITCOMMONCONTROLSEX common{sizeof(common),ICC_BAR_CLASSES};InitCommonControlsEx(&common);
    e.window=CreateWindowExW(0,kClass,L"骨骼变换",WS_CHILD|WS_CLIPCHILDREN|(popup?WS_VISIBLE|WS_CAPTION|WS_SYSMENU|WS_VSCROLL:0),20,10,650,820,parent,nullptr,GetModuleHandleW(nullptr),&e);
    e.title=Control(e,L"STATIC",popup?L"节点 · 多骨骼变换":L"骨骼变换 · 角色动画",SS_LEFT);
    e.intro=Control(e,L"STATIC",L"简化体型按身体部位调整；骨骼详情保留局部变换、惯性抖动和子骨骼模式。节点与技能预览使用同一组参数。缺失骨骼会跳过。",SS_LEFT);
    e.debug_on=Control(e,L"BUTTON",L"骨骼调试 · 显示完整骨架",BS_AUTOCHECKBOX|WS_TABSTOP,DebugOn);
    e.front=Control(e,L"BUTTON",L"正面 / 全身",BS_PUSHBUTTON,Front);e.side=Control(e,L"BUTTON",L"侧面 / 全身",BS_PUSHBUTTON,Side);e.back=Control(e,L"BUTTON",L"背面 / 全身",BS_PUSHBUTTON,Back);
    e.view=Control(e,L"STATIC",L"",SS_NOTIFY,View);
    e.on=Control(e,L"BUTTON",L"启用",BS_AUTOCHECKBOX|WS_TABSTOP,On);e.feet=Control(e,L"BUTTON",L"保留腿部原姿势",BS_AUTOCHECKBOX|WS_TABSTOP,Feet);
    e.simple=Control(e,L"BUTTON",L"体型",BS_PUSHBUTTON|WS_TABSTOP,Simple);e.detail=Control(e,L"BUTTON",L"骨骼",BS_PUSHBUTTON|WS_TABSTOP,Detail);
    e.character=Control(e,L"STATIC",L"",SS_LEFT);e.presets_tab=Control(e,L"BUTTON",L"预设",BS_PUSHBUTTON|WS_TABSTOP,PresetsTab);e.status_tab=Control(e,L"BUTTON",L"状态",BS_PUSHBUTTON|WS_TABSTOP,StatusTab);e.precise=Control(e,L"BUTTON",L"数值输入",BS_AUTOCHECKBOX|WS_TABSTOP,Precise);
    e.debug_tab=Control(e,L"BUTTON",L"骨架调试",BS_PUSHBUTTON|WS_TABSTOP,DebugTab);
    for(int i=0;i<4;++i){const wchar_t* names[]={L"▸  位移 · cm",L"▸  旋转 · 度",L"▾  缩放 · 倍率",L"▸  动态 · 惯性 / 胯部"};e.sections[i]=Control(e,L"BUTTON",names[i],BS_PUSHBUTTON|BS_LEFT|WS_TABSTOP,Section+i);SetPropW(e.sections[i],L"PBZBoneSection",reinterpret_cast<HANDLE>(1));e.section_reset[i]=Control(e,L"BUTTON",L"重置",BS_PUSHBUTTON|WS_TABSTOP,SectionReset+i);}
    e.group=Control(e,L"COMBOBOX",L"",CBS_DROPDOWNLIST|WS_VSCROLL|WS_TABSTOP,Group);e.group_info=Control(e,L"STATIC",L"",SS_LEFT);
    for(size_t i=0;i<3;++i){e.group_labels[i]=Control(e,L"STATIC",i==0?L"局部 X · 长度":i==1?L"局部 Y · 宽度":L"局部 Z · 深度",SS_LEFT);e.group_fields[i]=Control(e,L"EDIT",L"1",ES_AUTOHSCROLL|WS_TABSTOP,GroupNumber+static_cast<int>(i));SetWindowSubclass(e.group_fields[i],NumberProc,6+i,0);
        e.sliders[i]=BoneSlider::Create(e.window,GroupSlider+static_cast<int>(i),e.group_fields[i],.1,3,.01);}
    e.group_apply=Control(e,L"BUTTON",L"应用部位缩放 · 0.1–3 倍",BS_PUSHBUTTON|WS_TABSTOP,GroupApply);
    e.group_uniform_label=Control(e,L"STATIC",L"等比缩放 · XYZ",SS_LEFT);e.group_uniform_field=Control(e,L"EDIT",L"1",ES_AUTOHSCROLL|WS_TABSTOP,GroupUniformNumber);
    SetWindowSubclass(e.group_uniform_field,NumberProc,9,0);e.group_uniform_slider=BoneSlider::Create(e.window,GroupUniformSlider,e.group_uniform_field,.1,3,.01);
    e.edited=Control(e,L"BUTTON",L"仅已调整",BS_AUTOCHECKBOX|WS_TABSTOP,Edited);
    e.children=Control(e,L"COMBOBOX",L"",CBS_DROPDOWNLIST|WS_VSCROLL|WS_TABSTOP,Children);
    for(const wchar_t* s:{L"子骨骼：继承变换和缩放",L"子骨骼：跟随形状，保持尺寸",L"仅当前骨骼：保留后代原姿势"})SendMessageW(e.children,CB_ADDSTRING,0,reinterpret_cast<LPARAM>(s));
    e.mirror=Control(e,L"BUTTON",L"按参考骨架镜像到另一侧",BS_PUSHBUTTON|WS_TABSTOP,Mirror);e.reset=Control(e,L"BUTTON",L"重置此行",BS_PUSHBUTTON|WS_TABSTOP,Reset);e.reset_all=Control(e,L"BUTTON",L"清空本组调整",BS_PUSHBUTTON|WS_TABSTOP,ResetAll);
    e.donor=Control(e,L"STATIC",L"胸臀骨骼补充：需要安装 PBZSoftBones 资源包。为当前角色加载带真实权重的独立网格；关闭后恢复原网格。女性预设自动使用补充骨骼，技能预览同样适配。",SS_LEFT);
    e.supplement=Control(e,L"BUTTON",L"启用胸臀骨骼补充 · 当前角色与技能预览",BS_AUTOCHECKBOX|WS_TABSTOP,Supplement);
    SendMessageW(e.supplement,BM_SETCHECK,BoneMotionRuntime::SupplementEnabled()?BST_CHECKED:BST_UNCHECKED,0);
    e.list=Control(e,L"LISTBOX",L"",LBS_NOTIFY|LBS_NOINTEGRALHEIGHT|WS_VSCROLL|WS_TABSTOP,List);SendMessageW(e.list,LB_SETITEMHEIGHT,0,UiLayout::For(e.window).S(28));
    e.add=Control(e,L"BUTTON",L"添加骨骼",BS_PUSHBUTTON|WS_TABSTOP,Add);e.remove=Control(e,L"BUTTON",L"删除所选",BS_PUSHBUTTON|WS_TABSTOP,Remove);e.preset=Control(e,L"BUTTON",L"载入女性预设",BS_PUSHBUTTON|WS_TABSTOP,Preset);
    e.search=Control(e,L"EDIT",L"",ES_AUTOHSCROLL|WS_TABSTOP,Search);SendMessageW(e.search,EM_SETCUEBANNER,TRUE,reinterpret_cast<LPARAM>(L"搜索身体骨骼（不含表情骨骼）"));
    e.bone=Control(e,L"COMBOBOX",L"",CBS_DROPDOWNLIST|WS_VSCROLL|WS_TABSTOP,Bone);e.row_on=Control(e,L"BUTTON",L"启用此行",BS_AUTOCHECKBOX|WS_TABSTOP,RowOn);
    e.motion=Control(e,L"COMBOBOX",L"",CBS_DROPDOWNLIST|WS_VSCROLL|WS_TABSTOP,Motion);
    for(const wchar_t* s:{L"静态变换",L"惯性跟随（胸部 / 臀部等）",L"胯部：随原动画转动",L"躯干：反向转动"})SendMessageW(e.motion,CB_ADDSTRING,0,reinterpret_cast<LPARAM>(s));
    for(const wchar_t* label:{L"位移 X",L"位移 Y",L"位移 Z",L"旋转 X",L"旋转 Y",L"旋转 Z",L"缩放 X",L"缩放 Y",L"缩放 Z",L"运动强度 0–2",L"惯性位移上限 cm",L"弹簧频率 Hz",L"阻尼比"}){
        const size_t i=e.fields.size();e.labels.push_back(Control(e,L"STATIC",label,SS_LEFT));HWND f=Control(e,L"EDIT",L"0",ES_AUTOHSCROLL|WS_TABSTOP,Number+static_cast<int>(i));SendMessageW(f,EM_SETLIMITTEXT,24,0);SetWindowSubclass(f,NumberProc,i,0);e.fields.push_back(f);
        const double lo=i<3?-100:i<6?-180:i<9?.1:i==11?.5:i==12?.1:0,hi=i<3?100:i<6?180:i<9?3:i==9?5:i==10?15:i==11?10:2;
        e.row_sliders.push_back(BoneSlider::Create(e.window,RowSlider+static_cast<int>(i),f,lo,hi,i<6?1:.01));}
    e.uniform_label=Control(e,L"STATIC",L"整体大小",SS_LEFT);e.uniform_field=Control(e,L"EDIT",L"1",ES_AUTOHSCROLL|WS_TABSTOP,UniformNumber);e.uniform_slider=BoneSlider::Create(e.window,UniformSlider,e.uniform_field,.1,3,.01);
    e.apply=Control(e,L"BUTTON",L"应用当前行",BS_PUSHBUTTON|WS_TABSTOP,Apply);e.status=Control(e,L"STATIC",L"进入场景后显示骨骼列表。",SS_LEFT);
    UiTheme::SetRole(e.title,UiTheme::Role::Section);UiTheme::SetRole(e.intro,UiTheme::Role::Hint);UiTheme::SetRole(e.status,UiTheme::Role::Card);UiTheme::SetRole(e.apply,UiTheme::Role::Primary);
    UiTheme::Apply(e.window);BoneEditorSkin::Apply(e.window);SetWindowTextW(e.intro,L"拖动预览 · 释放保存；勾选「数值输入」可精确编辑。无需调整时关闭本组。 ");SetPropW(e.character,L"PBZBoneMuted",reinterpret_cast<HANDLE>(1));SetWindowSubclass(e.view,ViewProc,1,reinterpret_cast<DWORD_PTR>(&e));e.pose=BoneMotionRuntime::Get();e.catalog=e.pose.bones;e.mesh=e.pose.mesh;e.ready=true;Fill(e);Layout(e);
}
void Refresh(Editor& e){const ULONGLONG now=GetTickCount64();if(now<e.next)return;e.next=now+(e.debug?33:500);const auto s=BoneMotionRuntime::Get();
    if(s.mesh!=e.mesh||s.bones!=e.catalog){e.mesh=s.mesh;e.catalog=s.bones;BoneList(e);Groups(e);Layout(e);}
    if(e.debug){e.pose=s;InvalidateRect(e.view,nullptr,FALSE);}
    const auto checked=s.supplement_enabled?BST_CHECKED:BST_UNCHECKED;if(SendMessageW(e.supplement,BM_GETCHECK,0,0)!=checked)SendMessageW(e.supplement,BM_SETCHECK,checked,0);
    if(now<e.status_next)return;e.status_next=now+500;
    std::wstring text=L"当前角色："+Wide(s.mesh)+L"  · 已处理 "+std::to_wstring(s.frames)+L" 帧\r\n";
    std::wstring missing;for(const auto& r:e.config.rows)if(r.on){bool found=false;for(const auto& n:s.bones)if(!_stricmp(n.c_str(),r.bone.c_str())||!_stricmp(n.c_str(),("pbz_"+r.bone).c_str())){found=true;break;}if(!found){if(!missing.empty())missing+=L"、";missing+=Wide(r.bone);}}
    text+=missing.empty()?L"对应骨骼可用。预览按预览角色自身骨架匹配。":L"当前角色缺失（跳过）："+missing;
    if(s.supplement_enabled)text+=L"\r\n骨骼补充："+(s.supplement_displays>0?L"已加载（带蒙皮权重）":Wide(s.supplement_note));
    if(UiLayout::Caption(e.status)!=text){SetWindowTextW(e.status,text.c_str());if(e.panel==4)Layout(e);}
}
}
HWND CreatePage(HWND parent){if(!page_){page_=std::make_unique<Editor>();page_->config=BoneMotionRuntime::Global();Create(*page_,parent,false);}return page_->window;}
void Tick(){if(page_&&IsWindowVisible(page_->window))Refresh(*page_);}
void Close(){if(popup_){DestroyWindow(popup_->window);popup_.reset();DebugDemand();}}
void Shutdown(){Close();if(page_){DestroyWindow(page_->window);page_.reset();}BoneEditorSkin::Shutdown();}
void Open(HWND page,int node,Host host){Close();auto e=std::make_unique<Editor>();e->node=node;e->owner=page;e->host=std::move(host);std::wstring title;if(!e->host.read||!e->host.read(e->config,title))return;Create(*e,GetParent(page),true);SetWindowTextW(e->title,title.c_str());
    RECT parent{};GetClientRect(GetParent(page),&parent);const auto m=UiLayout::For(page);SetWindowPos(e->window,HWND_TOP,m.S(30),m.S(12),(std::min)(m.S(960),static_cast<int>(parent.right)-m.S(40)),(std::min)(m.S(820),static_cast<int>(parent.bottom)-m.S(24)),SWP_SHOWWINDOW);popup_=std::move(e);}
int EditedNode(){return popup_?popup_->node:-1;}
void TickEditor(HWND page){if(!popup_)return;BoneMotion::Config current;std::wstring title;
    if(!IsWindowVisible(page)||!popup_->host.read||!popup_->host.read(current,title)){Close();return;}Refresh(*popup_);}
}
