#include "ui_i18n.h"
#include "ui_i18n_strings.h"
#include <commctrl.h>
#include <algorithm>
#include <cwchar>
#include <map>
#include <vector>

namespace UiI18n {
namespace {
constexpr UINT_PTR kSubclass=0x50425a49;
struct Content { std::wstring text,cue; bool has_text=false,has_cue=false; std::vector<std::wstring> items; std::map<std::pair<int,int>,std::wstring> cells; std::vector<std::wstring> columns; };
std::map<HWND,Content> contents;
Language language_=Language::SimplifiedChinese;
std::wstring config_;
bool rebuilding_=false;
using Dictionary=std::map<std::wstring_view,std::wstring_view>;
Dictionary forward,reverse;
std::map<wchar_t,std::vector<const UiTranslation*>> phrases;
bool ready_=false;
void Dictionaries() {
    if (ready_) return;
    for (const auto& entry:kUiTranslations) {
        if (!entry.zh||!entry.en||!entry.zh[0]||!entry.en[0]) continue;
        forward.emplace(entry.zh,entry.en); reverse.emplace(entry.en,entry.zh);
        if (std::wcslen(entry.zh)>=2) phrases[entry.zh[0]].push_back(&entry);
    }
    for (auto& group:phrases) std::sort(group.second.begin(),group.second.end(),[](const UiTranslation* a,const UiTranslation* b){return std::wcslen(a->zh)>std::wcslen(b->zh);});
    ready_=true;
}
bool Editable(HWND control) {
    wchar_t type[64]{}; GetClassNameW(control,type,64);
    return !_wcsicmp(type,L"EDIT")&&!(GetWindowLongPtrW(control,GWL_STYLE)&ES_READONLY);
}
LRESULT CALLBACK Cleanup(HWND control,UINT message,WPARAM wparam,LPARAM lparam,UINT_PTR,DWORD_PTR) {
    if (message==WM_NCDESTROY) { contents.erase(control); RemoveWindowSubclass(control,Cleanup,kSubclass); }
    else if (!rebuilding_) {
        auto found=contents.find(control);
        if (found!=contents.end()) {
            if (message==LB_RESETCONTENT||message==CB_RESETCONTENT) found->second.items.clear();
            else if (message==LVM_DELETEALLITEMS) found->second.cells.clear();
        }
    }
    return DefSubclassProc(control,message,wparam,lparam);
}
Content& Track(HWND control) { SetWindowSubclass(control,Cleanup,kSubclass,0); return contents[control]; }
void Write(HWND control,const std::wstring& text) {
    const int length=GetWindowTextLengthW(control);
    if (length>=0&&static_cast<size_t>(length)==text.size()) {
        std::wstring current(static_cast<size_t>(length)+1,L'\0'); GetWindowTextW(control,current.data(),length+1); current.resize(static_cast<size_t>(length));
        if (current==text) return; // Preserve read-only selection/scroll during unchanged snapshot refreshes.
    }
    SetWindowTextW(control,text.c_str());
}
void Localize(HWND control) {
    const auto found=contents.find(control); if (found==contents.end()||!IsWindow(control)) return;
    Content& entry=found->second;
    if (entry.has_text&&!Editable(control)) { const auto text=Translate(entry.text); Write(control,text); }
    if (entry.has_cue) { const auto text=Translate(entry.cue); SendMessageW(control,EM_SETCUEBANNER,TRUE,reinterpret_cast<LPARAM>(text.c_str())); }
    if (!entry.items.empty()) {
        wchar_t type[64]{}; GetClassNameW(control,type,64);
        const bool combo=!_wcsicmp(type,L"COMBOBOX"),list=!_wcsicmp(type,L"LISTBOX");
        if (combo||list) {
            const UINT count_message=combo?CB_GETCOUNT:LB_GETCOUNT,selection_message=combo?CB_GETCURSEL:LB_GETCURSEL;
            const LRESULT count=SendMessageW(control,count_message,0,0),selection=SendMessageW(control,selection_message,0,0);
            if (count>=0&&static_cast<size_t>(count)==entry.items.size()) {
                const LRESULT top=SendMessageW(control,combo?CB_GETTOPINDEX:LB_GETTOPINDEX,0,0);
                std::vector<LRESULT> data; data.reserve(entry.items.size());
                for (size_t i=0;i<entry.items.size();++i) data.push_back(SendMessageW(control,combo?CB_GETITEMDATA:LB_GETITEMDATA,i,0));
                const auto source=entry.items;
                std::vector<std::pair<std::wstring,LRESULT>> ordered;
                const bool selected=selection>=0&&static_cast<size_t>(selection)<source.size();
                SendMessageW(control,WM_SETREDRAW,FALSE,0); SendMessageW(control,combo?CB_RESETCONTENT:LB_RESETCONTENT,0,0);
                for (size_t i=0;i<source.size();++i) {
                    const auto text=Translate(source[i]); const LRESULT row=SendMessageW(control,combo?CB_ADDSTRING:LB_ADDSTRING,0,reinterpret_cast<LPARAM>(text.c_str()));
                    if (row>=0) {
                        SendMessageW(control,combo?CB_SETITEMDATA:LB_SETITEMDATA,static_cast<WPARAM>(row),data[i]);
                        ordered.insert(ordered.begin()+row,{source[i],data[i]});
                    }
                }
                entry.items.clear(); LRESULT new_selection=-1;
                for (size_t i=0;i<ordered.size();++i) {
                    entry.items.push_back(ordered[i].first);
                    if (selected&&ordered[i].first==source[static_cast<size_t>(selection)]&&ordered[i].second==data[static_cast<size_t>(selection)]) new_selection=static_cast<LRESULT>(i);
                }
                SendMessageW(control,combo?CB_SETCURSEL:LB_SETCURSEL,static_cast<WPARAM>(new_selection),0);
                if (top>=0) SendMessageW(control,combo?CB_SETTOPINDEX:LB_SETTOPINDEX,static_cast<WPARAM>(top),0);
                SendMessageW(control,WM_SETREDRAW,TRUE,0); InvalidateRect(control,nullptr,TRUE);
            }
        }
    }
    for (const auto& cell:entry.cells) {
        if (cell.first.first>=ListView_GetItemCount(control)) continue;
        const auto text=Translate(cell.second);
        ListView_SetItemText(control,cell.first.first,cell.first.second,const_cast<wchar_t*>(text.c_str()));
    }
    for (size_t i=0;i<entry.columns.size();++i) {
        const auto text=Translate(entry.columns[i]); HDITEMW item{}; item.mask=HDI_TEXT; item.pszText=const_cast<wchar_t*>(text.c_str());
        Header_SetItem(control,static_cast<int>(i),&item);
    }
}
BOOL CALLBACK Headers(HWND control,LPARAM) {
    wchar_t type[64]{}; GetClassNameW(control,type,64);
    if (_wcsicmp(type,WC_HEADERW)) return TRUE;
    Content& entry=Track(control);
    const int count=Header_GetItemCount(control);
    if (entry.columns.empty()&&count>0) for (int i=0;i<count;++i) {
        wchar_t text[512]{}; HDITEMW item{}; item.mask=HDI_TEXT; item.pszText=text; item.cchTextMax=512;
        if (Header_GetItem(control,i,&item)) entry.columns.emplace_back(text); else entry.columns.emplace_back();
    }
    Localize(control); return TRUE;
}
}
void Initialize(const wchar_t* file) {
    Dictionaries(); config_=file?file:L"";
    if (!config_.empty()) {
        wchar_t value[32]{}; GetPrivateProfileStringW(L"UI",L"Language",L"zh-CN",value,32,config_.c_str());
        language_=!_wcsicmp(value,L"en-US")?Language::English:Language::SimplifiedChinese;
    }
}
Language GetLanguage() { return language_; }
std::wstring Translate(std::wstring_view source) {
    Dictionaries();
    if (language_==Language::SimplifiedChinese) {
        const auto exact=reverse.find(source); return std::wstring(exact!=reverse.end()?exact->second:source);
    }
    const auto exact=forward.find(source); if (exact!=forward.end()) return std::wstring(exact->second);
    std::wstring result; result.reserve(source.size());
    for (size_t at=0;at<source.size();) {
        const UiTranslation* match=nullptr;
        const auto group=phrases.find(source[at]);
        if (group!=phrases.end()) for (const auto* candidate:group->second) {
            const std::wstring_view key(candidate->zh);
            if (source.substr(at,key.size())==key) { match=candidate; break; }
        }
        if (!match) { result+=source[at++]; continue; }
        const std::wstring_view value(match->en);
        const auto letter=[](wchar_t c) { return (c>=L'a'&&c<=L'z')||(c>=L'A'&&c<=L'Z')||(c>=L'0'&&c<=L'9'); };
        if (!result.empty()&&!value.empty()&&letter(result.back())&&letter(value.front())) result+=L' ';
        result+=value; at+=std::wcslen(match->zh);
    }
    return result;
}
void SetLanguage(Language language) {
    language_=language;
    if (!config_.empty()) WritePrivateProfileStringW(L"UI",L"Language",language==Language::English?L"en-US":L"zh-CN",config_.c_str());
    rebuilding_=true;
    std::vector<HWND> windows; for (const auto& item:contents) windows.push_back(item.first);
    for (const HWND window:windows) Localize(window);
    rebuilding_=false;
}
void Bind(HWND control,const wchar_t* source) {
    if (!control||!IsWindow(control)||Editable(control)) return;
    Content& entry=Track(control); entry.text=source?source:L""; entry.has_text=true;
    const auto text=Translate(entry.text); Write(control,text);
}
void SetText(HWND control,const wchar_t* source) {
    if (!control||!IsWindow(control)) return;
    if (Editable(control)) { SetWindowTextW(control,source?source:L""); return; }
    Bind(control,source);
}
LRESULT AddString(HWND control,UINT message,WPARAM parameter,const wchar_t* source) {
    if (!control||!IsWindow(control)||!source||(message!=CB_ADDSTRING&&message!=LB_ADDSTRING)) return -1;
    Content& entry=Track(control); const auto text=Translate(source);
    const LRESULT result=SendMessageW(control,message,parameter,reinterpret_cast<LPARAM>(text.c_str()));
    if (result>=0) {
        const size_t at=static_cast<size_t>(result);
        if (at<=entry.items.size()) entry.items.insert(entry.items.begin()+static_cast<ptrdiff_t>(at),source);
        else entry.items.emplace_back(source);
    }
    return result;
}
void SetCell(HWND list,int row,int column,const wchar_t* source) {
    if (!list||!IsWindow(list)||row<0||column<0) return;
    Track(list).cells[{row,column}]=source?source:L"";
    const auto text=Translate(source?source:L""); ListView_SetItemText(list,row,column,const_cast<wchar_t*>(text.c_str()));
}
void Cue(HWND control,const wchar_t* source) {
    if (!control||!IsWindow(control)) return;
    Content& entry=Track(control); entry.cue=source?source:L""; entry.has_cue=true;
    const auto text=Translate(entry.cue); SendMessageW(control,EM_SETCUEBANNER,TRUE,reinterpret_cast<LPARAM>(text.c_str()));
}
void Apply(HWND root) {
    if (!root) return;
    rebuilding_=true; EnumChildWindows(root,Headers,0);
    std::vector<HWND> windows; for (const auto& entry:contents) if (entry.first==root||IsChild(root,entry.first)) windows.push_back(entry.first);
    for (const HWND window:windows) Localize(window);
    rebuilding_=false;
}
void Shutdown() {
    for (const auto& entry:contents) if (IsWindow(entry.first)) RemoveWindowSubclass(entry.first,Cleanup,kSubclass);
    contents.clear(); config_.clear();
}
}
