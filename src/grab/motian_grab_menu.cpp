#include "motian_grab_menu.h"
#include "debug_hub.h"
#include "grab_extras.h"
#include "ui_i18n.h"
#include "ui_layout.h"
#include "ui_theme.h"
#include <windows.h>
#include <algorithm>
#include <cmath>
#include <cwchar>
#include <string>

namespace MotianGrabMenu {
namespace {
constexpr wchar_t kClass[]=L"PBZMotianGrabWindow";
constexpr int kEnable=101,kVariant=102,kRange=103,kApply=104,kTrigger=105,kCancel=106,kBoss=107,kPosition=108,kSourceBoss=109,kRecommended=110;
constexpr int kHideWeapon=111,kDamageMode=112,kDamageValue=113,kInvulnerable=114,kTangExecution=115;
HWND window_=nullptr,enabled_=nullptr,boss_=nullptr,position_=nullptr,tang_=nullptr,variant_=nullptr,range_=nullptr,range_label_=nullptr,status_=nullptr,detail_=nullptr;
HWND variant_label_=nullptr,apply_=nullptr,trigger_=nullptr,cancel_=nullptr,status_section_=nullptr,detail_section_=nullptr;
HWND source_label_=nullptr,source_boss_=nullptr,source_status_=nullptr;
HWND range_hint_=nullptr,recommended_=nullptr;
// Grab add-ons (GrabExtras): weapon hiding and damage, saved with the other settings.
HWND extras_section_=nullptr,hide_weapon_=nullptr,damage_label_=nullptr,damage_mode_=nullptr,damage_value_=nullptr,damage_unit_=nullptr,invulnerable_=nullptr,extras_result_=nullptr;
std::wstring source_filter_,catalog_signature_;
HFONT font_=nullptr;
MotianGrab::Logger logger_=nullptr;
bool initialized_=false,rendering_=false;
ULONGLONG next_refresh_=0;
HWND Control(const wchar_t* cls,const wchar_t* text,DWORD style,int x,int y,int w,int h,int id) {
    HWND control=CreateWindowExW(cls==std::wstring(L"EDIT")?WS_EX_CLIENTEDGE:0,cls,text,WS_CHILD|WS_VISIBLE|style,
        x,y,w,h,window_,reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),GetModuleHandleW(nullptr),nullptr);
    if (control) SendMessageW(control,WM_SETFONT,reinterpret_cast<WPARAM>(font_),TRUE);
    if (control&&(_wcsicmp(cls,L"EDIT")||(style&ES_READONLY))) UiI18n::Bind(control,text);
    return control;
}
int32_t SelectedVariant() {
    const LRESULT selected=SendMessageW(variant_,CB_GETCURSEL,0,0);
    return selected==CB_ERR?-1:static_cast<int32_t>(SendMessageW(variant_,CB_GETITEMDATA,selected,0));
}
const MotianGrab::Variant* FindVariant(const MotianGrab::Snapshot& state,int32_t index) {
    for (const auto& row:state.variants) if (row.index==index) return &row;
    return nullptr;
}
void RebuildMoves(const MotianGrab::Snapshot& state,int32_t preferred) {
    SendMessageW(variant_,CB_RESETCONTENT,0,0);
    LRESULT selected=CB_ERR,first_supported=CB_ERR;
    for (const auto& move:state.variants) {
        if (!source_filter_.empty()&&move.boss_id!=source_filter_) continue;
        std::wstring label=move.name;
        if (source_filter_.empty()&&!move.boss_name.empty()) label=move.boss_name+L" · "+label;
        if (!move.supported) label+=L" · 待适配";
        const LRESULT row=UiI18n::AddString(variant_,CB_ADDSTRING,0,label.c_str());
        if (row==CB_ERR||row==CB_ERRSPACE) continue;
        SendMessageW(variant_,CB_SETITEMDATA,row,move.index);
        if (move.index==preferred) selected=row;
        if (move.supported&&first_supported==CB_ERR) first_supported=row;
    }
    if (selected==CB_ERR) selected=first_supported;
    if (selected==CB_ERR&&SendMessageW(variant_,CB_GETCOUNT,0,0)>0) selected=0;
    SendMessageW(variant_,CB_SETCURSEL,selected,0);
}
void RebuildCatalog(const MotianGrab::Snapshot& state,bool force) {
    std::wstring signature;
    for (const auto& boss:state.bosses) signature+=boss.id+L"|"+boss.name+L"|"+boss.status+L"|"+std::to_wstring(boss.move_count)+L";";
    for (const auto& move:state.variants) signature+=move.id+L"|"+std::to_wstring(move.index)+(move.supported?L"+":L"-")+move.boss_id+move.name+move.detail+move.reason;
    if (!force&&signature==catalog_signature_) return;
    catalog_signature_=std::move(signature);
    const int32_t preferred=force?state.options.variant:SelectedVariant();
    if (force) source_filter_.clear();
    SendMessageW(source_boss_,CB_RESETCONTENT,0,0);
    LRESULT selected=UiI18n::AddString(source_boss_,CB_ADDSTRING,0,L"全部 Boss");
    SendMessageW(source_boss_,CB_SETITEMDATA,selected,0);
    for (size_t i=0;i<state.bosses.size();++i) {
        const auto& boss=state.bosses[i];std::wstring label=boss.name;
        label+=L" · "+std::to_wstring(boss.move_count)+L" 招";
        if (!boss.status.empty()) label+=L" · "+boss.status;
        const LRESULT row=UiI18n::AddString(source_boss_,CB_ADDSTRING,0,label.c_str());
        if (row==CB_ERR||row==CB_ERRSPACE) continue;
        SendMessageW(source_boss_,CB_SETITEMDATA,row,static_cast<LPARAM>(i+1));
        if (boss.id==source_filter_) selected=row;
    }
    SendMessageW(source_boss_,CB_SETCURSEL,selected,0);
    if (selected==0) source_filter_.clear();
    RebuildMoves(state,preferred);
}
void Apply() {
    if (rendering_) return;
    MotianGrab::Options options=MotianGrab::GetSnapshot().options;
    options.enabled=SendMessageW(enabled_,BM_GETCHECK,0,0)==BST_CHECKED;
    options.allow_bosses=SendMessageW(boss_,BM_GETCHECK,0,0)==BST_CHECKED;
    options.position_correction=SendMessageW(position_,BM_GETCHECK,0,0)==BST_CHECKED;
    options.tang_execution=SendMessageW(tang_,BM_GETCHECK,0,0)==BST_CHECKED;
    const auto state=MotianGrab::GetSnapshot();
    const auto* selected=FindVariant(state,SelectedVariant());
    if (options.enabled) {
        if (!selected||!selected->supported) {
            UiI18n::SetText(status_,L"请先选择已适配的投技招式；未提交设置");return;
        }
        options.variant=selected->index;
    }
    wchar_t text[64]{}; GetWindowTextW(range_,text,64); wchar_t* end=nullptr; const double value=std::wcstod(text,&end);
    const auto* range_move=options.enabled?selected:FindVariant(state,options.variant);
    const double maximum=range_move?range_move->max_range:400.0;
    if (end==text || *end || !std::isfinite(value) || value<80 || value>maximum) {
        wchar_t message[128]{};swprintf_s(message,L"距离需为 80–%.0f 厘米；尚未提交设置",maximum);
        UiI18n::SetText(status_,message); return;
    }
    options.range=static_cast<float>(value);
    GrabExtras::Options extras=GrabExtras::GetOptions();
    extras.hide_weapon=SendMessageW(hide_weapon_,BM_GETCHECK,0,0)==BST_CHECKED;
    extras.ignore_invulnerable=SendMessageW(invulnerable_,BM_GETCHECK,0,0)==BST_CHECKED;
    const LRESULT mode=SendMessageW(damage_mode_,CB_GETCURSEL,0,0);
    extras.damage=mode==1?GrabExtras::Damage::Multiplier:mode==2?GrabExtras::Damage::Percent:GrabExtras::Damage::Native;
    if (extras.damage!=GrabExtras::Damage::Native) {
        wchar_t amount_text[64]{}; GetWindowTextW(damage_value_,amount_text,64); wchar_t* amount_end=nullptr; const double amount=std::wcstod(amount_text,&amount_end);
        const bool percent=extras.damage==GrabExtras::Damage::Percent;
        if (amount_end==amount_text||*amount_end||!std::isfinite(amount)||amount<(percent?1:.1)||amount>(percent?100:10)) {
            UiI18n::SetText(status_,percent?L"伤害百分比需为 1–100；尚未提交设置":L"伤害倍率需为 0.1–10；尚未提交设置"); return;
        }
        (percent?extras.percent:extras.multiplier)=static_cast<float>(amount);
    }
    GrabExtras::SetOptions(extras);
    if (!MotianGrab::RequestOptions(options)) UiI18n::SetText(status_,L"投技设置未接受；请等待当前动作清理后重试");
}
void ShowDamageValue(const GrabExtras::Options& extras,int mode) {
    wchar_t value[32]{};
    if (mode==2) swprintf_s(value,L"%.0f",static_cast<double>(extras.percent)); else swprintf_s(value,L"%.2g",static_cast<double>(extras.multiplier));
    SetWindowTextW(damage_value_,value);
    UiI18n::SetText(damage_unit_,mode==2?L"% 最大生命":mode==1?L"× 原生伤害":L"");
    EnableWindow(damage_value_,mode!=0);
}
void Refresh(bool options) {
    if (!window_) return; const MotianGrab::Snapshot state=MotianGrab::GetSnapshot(); rendering_=true;
    if (options) {
        SendMessageW(enabled_,BM_SETCHECK,state.options.enabled?BST_CHECKED:BST_UNCHECKED,0);
        SendMessageW(boss_,BM_SETCHECK,state.options.allow_bosses?BST_CHECKED:BST_UNCHECKED,0);
        SendMessageW(position_,BM_SETCHECK,state.options.position_correction?BST_CHECKED:BST_UNCHECKED,0);
        SendMessageW(tang_,BM_SETCHECK,state.options.tang_execution?BST_CHECKED:BST_UNCHECKED,0);
        wchar_t value[32]{}; swprintf_s(value,L"%.0f",state.options.range); SetWindowTextW(range_,value);
        const GrabExtras::Options extras=GrabExtras::GetOptions();
        SendMessageW(hide_weapon_,BM_SETCHECK,extras.hide_weapon?BST_CHECKED:BST_UNCHECKED,0);
        SendMessageW(invulnerable_,BM_SETCHECK,extras.ignore_invulnerable?BST_CHECKED:BST_UNCHECKED,0);
        SendMessageW(damage_mode_,CB_SETCURSEL,static_cast<int>(extras.damage),0);
        ShowDamageValue(extras,static_cast<int>(extras.damage));
    }
    UiI18n::SetText(extras_result_,GrabExtras::LastResult().c_str());
    RebuildCatalog(state,options);
    std::wstring source_status=L"选择来源 Boss，再选择它的投技；先应用设置，等待预载完成后触发。";
    for (const auto& boss:state.bosses) if (boss.id==source_filter_) {
        source_status=boss.name+L" · "+boss.status;
        if (!boss.move_count) source_status+=L"\r\n当前没有已适配的投技招式，保留在列表供核对。";
        break;
    }
    UiI18n::SetText(source_status_,source_status.c_str());
    std::wstring status=state.status+L"\r\n";
    status+=L"投技资源："+std::wstring(!state.options.enabled?L"已关闭":state.prepared?L"已预载":state.preparing?L"准备中，完成后再按组合键":L"尚未就绪");
    if (!state.preparation_status.empty()) status+=L" — "+state.preparation_status;
    if (state.preparation_last_ms>0) {
        wchar_t elapsed[64]{}; swprintf_s(elapsed,L"（本步骤 %.1f ms）",static_cast<double>(state.preparation_last_ms)); status+=elapsed;
    }
    status+=L"\r\n施放条件："+std::wstring(state.ready?L"角色与资源已就绪":L"等待角色或投技资源")+
        L"　会话："+(state.active?std::wstring(L"运行中"):std::wstring(L"空闲"))+L"　清理："+(state.cleanup_pending?std::wstring(L"待完成，切图受阻"):std::wstring(L"无残留"));
    if (state.disabled) status+=L"\r\n模块调用异常已达到保护上限，请关闭功能并完成清理。";
    if (!state.target.empty()) status+=L"\r\n目标："+state.target;
    const auto* applied=FindVariant(state,state.options.variant);
    if (applied) status+=L"\r\n已应用招式："+applied->boss_name+L" · "+applied->name;
    status+=L"\r\n请求 / 双能力激活："+std::to_wstring(state.attempts)+L" / "+std::to_wstring(state.activations);
    status+=L"　会话类引用："+std::to_wstring(state.retained_classes);
    if (!state.alignment_status.empty()) status+=L"\r\n位置："+state.alignment_status;
    if (state.options.tang_execution) {
        status+=std::wstring(L"\r\n")+(state.execution_prepared?L"黑伤处决：魔渊 · 完整擒拿已预载":L"黑伤处决：魔渊 · 完整擒拿准备中")+
            L"；已替换处决次数："+std::to_wstring(state.executions_redirected);
        if (!state.execution_status.empty()) status+=L"\r\n"+state.execution_status;
    }
    UiI18n::SetText(status_,status.c_str());
    const auto* selected=FindVariant(state,SelectedVariant());
    const bool selectable=selected&&selected->supported;
    wchar_t range_hint[160]{};
    if (selected) swprintf_s(range_hint,L"此招式距离：80–%.0f 厘米；推荐 %.0f 厘米",static_cast<double>(selected->max_range),static_cast<double>(selected->recommended_range));
    UiI18n::SetText(range_hint_,range_hint);
    EnableWindow(recommended_,selectable);
    EnableWindow(apply_,!state.active&&!state.cleanup_pending&&!state.disabled&&(!SendMessageW(enabled_,BM_GETCHECK,0,0)||selectable));
    EnableWindow(GetDlgItem(window_,kTrigger),state.options.enabled&&state.prepared&&!state.active&&!state.cleanup_pending&&!state.disabled&&selectable&&selected->index==state.options.variant);
    std::wstring detail=L"PS5：三角 + 圈（220 毫秒内）；每次触发后须松开两键。\r\n"
        L"开启或更换招式后先等待资源显示“已预载”，再回到游戏按组合键。资源准备耗时会单独显示。\r\n"
        L"以原生成对能力 / Timeline 负责受投、伤害和位移；关闭或取消走双方原能力结束清理。\r\n"
        L"投技附加：可在投技期间隐藏主角武器（结束后恢复原显示），并在原投技结束后按倍率或最大生命百分比补足伤害；伤害经游戏属性接口写入。未开启“无视无敌”时，只在目标本次确实受到原生伤害时追加。\r\n"
        L"位置修正：先验证地面。直线对位招式只在安全距离内修正主角一次；其他招式保留原生参考系和完整位移曲线。不移动目标，可关闭对比。\r\n"
        L"每招的搜索距离和推荐距离不同；使用推荐距离只填写编辑框，应用设置后生效。\r\n"
        L"支持范围：地面近处敌对、存活且可受投的人形敌人；人形小怪在受击硬直或特殊动作中也可投。\r\n"
        L"黑伤处决：持黑伤处决时，原处决在开始时取消，改为对同一目标施放魔渊 · 完整擒拿（不受“开启投技”开关影响；伤害按投技附加设置）。目标不适配时保留原处决。\r\n"
        L"人形 Boss 可由独立选项开启；仍检查体型、受投条件、双方状态和结束恢复。巨型、动物、友军及剧情状态不触发。\r\n"
        L"不更换主角 Actor / ASC / 进度；原 Hun 与 NPC 外观皆可尝试，动作和武器适配仍需实机验证。";
    if (selected) {
        detail+=L"\r\n所选来源："+selected->boss_name+L"\r\n所选招式："+selected->name+L"\r\n"+selected->detail;
        if (!selected->reason.empty()) detail+=L"\r\n适配状态："+selected->reason;
    } else detail+=L"\r\n当前来源没有可选投技。";
    UiI18n::SetText(detail_,detail.c_str()); rendering_=false;
}
void Layout() {
    if (!window_||!detail_section_) return;
    const UiLayout::Metrics m=UiLayout::For(window_);
    RECT client{}; GetClientRect(window_,&client);
    const int margin=m.Margin(),gap=m.Gap(),x=margin,width=(std::max)(m.S(600),static_cast<int>(client.right)-2*margin);
    int y=margin;
    UiLayout::Flow flow(x,y,width,m.Check(),gap*3,gap/2);
    flow.Add(enabled_,UiLayout::CheckWidth(enabled_,m)); flow.Add(boss_,UiLayout::CheckWidth(boss_,m));
    flow.Break(); flow.Add(position_,UiLayout::CheckWidth(position_,m));
    flow.Break(); flow.Add(tang_,UiLayout::CheckWidth(tang_,m));
    y=flow.Bottom()+gap;
    UiLayout::Flow source_row(x,y,width,m.Row(),gap,gap);
    source_row.Add(source_label_,UiLayout::TextWidth(source_label_),m.Line());
    source_row.AddCombo(source_boss_,(std::max)(m.S(300),width-UiLayout::TextWidth(source_label_)-gap),m.S(300));
    y=source_row.Bottom()+gap/2;
    const int source_height=UiLayout::TextHeight(source_status_,width,2);
    UiLayout::Place(source_status_,x,y,width,source_height);y+=source_height+gap;
    UiLayout::Flow row(x,y,width,m.Row(),gap,gap);
    const int variant_label=UiLayout::TextWidth(variant_label_),range_label=UiLayout::TextWidth(range_label_);
    row.Add(variant_label_,variant_label,m.Line());
    row.AddCombo(variant_,(std::max)(m.S(260),width-variant_label-range_label-m.S(100)-gap*5),m.S(260));
    row.Space(gap); row.Add(range_label_,range_label,m.Line()); row.Add(range_,m.S(100));
    y=row.Bottom()+gap;
    UiLayout::Flow range_row(x,y,width,m.Row(),gap,gap);
    const int recommended_width=(std::max)(m.S(150),UiLayout::ButtonWidth(recommended_,m));
    range_row.Add(range_hint_,(std::max)(m.S(260),width-recommended_width-gap),m.Line());
    range_row.Add(recommended_,recommended_width);
    y=range_row.Bottom()+gap;
    // Grab add-ons: weapon hiding, damage mode / value, invulnerability.
    UiLayout::Place(extras_section_,x,y,width,m.Section()); y+=m.Section()+gap/2;
    UiLayout::Flow extras_row(x,y,width,m.Check(),gap*3,gap/2);
    extras_row.Add(hide_weapon_,UiLayout::CheckWidth(hide_weapon_,m)); extras_row.Add(invulnerable_,UiLayout::CheckWidth(invulnerable_,m));
    y=extras_row.Bottom()+gap/2;
    UiLayout::Flow damage_row(x,y,width,m.Row(),gap,gap);
    damage_row.Add(damage_label_,UiLayout::TextWidth(damage_label_),m.Line());
    damage_row.AddCombo(damage_mode_,m.S(220),m.S(220));
    damage_row.Add(damage_value_,m.S(90));
    damage_row.Add(damage_unit_,(std::max)(m.S(110),UiLayout::TextWidth(damage_unit_)),m.Line());
    y=damage_row.Bottom()+gap/2;
    const int result_height=UiLayout::TextHeight(extras_result_,width,2);
    UiLayout::Place(extras_result_,x,y,width,result_height); y+=result_height+gap;
    y=UiLayout::ButtonRow({apply_,trigger_,cancel_},x,y,width,m,m.S(150))+gap*2;
    // Live status and the long description share the remaining height.
    const int minimum_status=5*m.Line()+m.S(14),minimum_detail=6*m.Line()+m.S(14);
    const int free=(std::max)(minimum_status+minimum_detail,static_cast<int>(client.bottom)-margin-y-2*(m.Section()+gap/2)-gap*2);
    const int status=(std::max)(minimum_status,free*9/20),detail=(std::max)(minimum_detail,free-status);
    UiLayout::Place(status_section_,x,y,width,m.Section()); y+=m.Section()+gap/2;
    UiLayout::Place(status_,x,y,width,status); y+=status+gap*2;
    UiLayout::Place(detail_section_,x,y,width,m.Section()); y+=m.Section()+gap/2;
    UiLayout::Place(detail_,x,y,width,detail); y+=detail+margin;
    DebugHub::SetMinimum(window_,m.S(600)+2*margin,y-status-detail+minimum_status+minimum_detail);
}
LRESULT CALLBACK WindowProc(HWND window,UINT message,WPARAM wparam,LPARAM lparam) {
    switch (message) {
    case WM_COMMAND:
        if (LOWORD(wparam)==IDCANCEL) { if (DebugHub::IsEmbedded(window)) DebugHub::Hide(); else ShowWindow(window,SW_HIDE); return 0; }
        if (LOWORD(wparam)==kApply && HIWORD(wparam)==BN_CLICKED) Apply();
        else if (LOWORD(wparam)==kTrigger && HIWORD(wparam)==BN_CLICKED) {
            const int32_t selected=SelectedVariant();
            if (selected>=0) MotianGrab::RequestTrigger(selected);
        }
        else if (LOWORD(wparam)==kCancel && HIWORD(wparam)==BN_CLICKED) MotianGrab::RequestCancel();
        else if (LOWORD(wparam)==kRecommended&&HIWORD(wparam)==BN_CLICKED) {
            const auto state=MotianGrab::GetSnapshot();const auto* selected=FindVariant(state,SelectedVariant());
            if (selected&&selected->supported) { wchar_t value[32]{};swprintf_s(value,L"%.0f",static_cast<double>(selected->recommended_range));SetWindowTextW(range_,value); }
        }
        else if (LOWORD(wparam)==kSourceBoss&&HIWORD(wparam)==CBN_SELCHANGE) {
            const auto state=MotianGrab::GetSnapshot();const LRESULT row=SendMessageW(source_boss_,CB_GETCURSEL,0,0);
            const LRESULT data=row==CB_ERR?0:SendMessageW(source_boss_,CB_GETITEMDATA,row,0);
            source_filter_=data>0&&static_cast<size_t>(data)<=state.bosses.size()?state.bosses[static_cast<size_t>(data-1)].id:L"";
            RebuildMoves(state,SelectedVariant());Refresh(false);Layout();
        }
        else if (LOWORD(wparam)==kVariant && HIWORD(wparam)==CBN_SELCHANGE) Refresh(false);
        else if (LOWORD(wparam)==kDamageMode && HIWORD(wparam)==CBN_SELCHANGE) {
            ShowDamageValue(GrabExtras::GetOptions(),static_cast<int>(SendMessageW(damage_mode_,CB_GETCURSEL,0,0))); Layout();
        }
        return 0;
    case WM_CLOSE: if (DebugHub::IsEmbedded(window)) DebugHub::Hide(); else ShowWindow(window,SW_HIDE); return 0;
    case WM_SIZE: Layout(); return 0;
    case WM_DPICHANGED_AFTERPARENT: font_=UiTheme::GetFont(UiTheme::Font::Body,window); UiLayout::UseFont(window,font_); Layout(); return 0;
    case WM_DESTROY: window_=nullptr; return 0;
    default: return DefWindowProcW(window,message,wparam,lparam);
    }
}
bool Create() {
    if (window_) return true;
    WNDCLASSEXW cls{}; cls.cbSize=sizeof(cls); cls.lpfnWndProc=&WindowProc; cls.hInstance=GetModuleHandleW(nullptr);
    cls.lpszClassName=kClass; cls.hCursor=LoadCursorW(nullptr,IDC_ARROW); cls.hbrBackground=reinterpret_cast<HBRUSH>(COLOR_BTNFACE+1);
    if (!RegisterClassExW(&cls) && GetLastError()!=ERROR_CLASS_ALREADY_EXISTS) return false;
    window_=CreateWindowExW(WS_EX_APPWINDOW,kClass,L"PBZ 投技 — PS5 三角 + 圈",WS_OVERLAPPED|WS_CAPTION|WS_SYSMENU|WS_MINIMIZEBOX,
        CW_USEDEFAULT,CW_USEDEFAULT,750,680,nullptr,nullptr,cls.hInstance,nullptr);
    if (!window_) return false;
    UiI18n::Bind(window_,L"PBZ 投技 — PS5 三角 + 圈");
    font_=UiTheme::GetFont(UiTheme::Font::Body,window_);
    enabled_=Control(L"BUTTON",L"开启投技",BS_AUTOCHECKBOX|WS_TABSTOP,0,0,10,10,kEnable);
    boss_=Control(L"BUTTON",L"目标允许人形 Boss（实验）",BS_AUTOCHECKBOX|WS_TABSTOP,0,0,10,10,kBoss);
    position_=Control(L"BUTTON",L"按原投技距离修正位置（一次；地面 / 碰撞验证）",BS_AUTOCHECKBOX|WS_TABSTOP,0,0,10,10,kPosition);
    tang_=Control(L"BUTTON",L"黑伤处决改为魔渊 · 完整擒拿",BS_AUTOCHECKBOX|WS_TABSTOP,0,0,10,10,kTangExecution);
    source_label_=Control(L"STATIC",L"来源 Boss",SS_LEFT,0,0,10,10,0);
    source_boss_=Control(L"COMBOBOX",L"",CBS_DROPDOWNLIST|WS_VSCROLL|WS_TABSTOP,0,0,10,10,kSourceBoss);
    source_status_=Control(L"STATIC",L"",SS_LEFT,0,0,10,10,0);
    SendMessageW(source_boss_,CB_SETDROPPEDWIDTH,UiLayout::For(window_).S(650),0);
    variant_label_=Control(L"STATIC",L"招式",0,0,0,10,10,0);
    variant_=Control(L"COMBOBOX",L"",CBS_DROPDOWNLIST|WS_VSCROLL|WS_TABSTOP,0,0,10,10,kVariant);
    range_label_=Control(L"STATIC",L"距离（厘米）",0,0,0,10,10,0);
    range_=Control(L"EDIT",L"280",ES_AUTOHSCROLL|WS_TABSTOP,0,0,10,10,kRange);
    range_hint_=Control(L"STATIC",L"",SS_LEFT,0,0,10,10,0);
    recommended_=Control(L"BUTTON",L"使用推荐距离",BS_PUSHBUTTON|WS_TABSTOP,0,0,10,10,kRecommended);
    extras_section_=Control(L"STATIC",L"投技附加",SS_LEFT,0,0,10,10,0);
    hide_weapon_=Control(L"BUTTON",L"投技时隐藏主角武器",BS_AUTOCHECKBOX|WS_TABSTOP,0,0,10,10,kHideWeapon);
    invulnerable_=Control(L"BUTTON",L"无视 Boss 无敌直接造成伤害",BS_AUTOCHECKBOX|WS_TABSTOP,0,0,10,10,kInvulnerable);
    damage_label_=Control(L"STATIC",L"投技伤害",0,0,0,10,10,0);
    damage_mode_=Control(L"COMBOBOX",L"",CBS_DROPDOWNLIST|WS_VSCROLL|WS_TABSTOP,0,0,10,10,kDamageMode);
    UiI18n::AddString(damage_mode_,CB_ADDSTRING,0,L"原生伤害");
    UiI18n::AddString(damage_mode_,CB_ADDSTRING,0,L"倍率（原生伤害 ×）");
    UiI18n::AddString(damage_mode_,CB_ADDSTRING,0,L"最大生命百分比");
    damage_value_=Control(L"EDIT",L"2",ES_AUTOHSCROLL|WS_TABSTOP,0,0,10,10,kDamageValue);
    damage_unit_=Control(L"STATIC",L"",0,0,0,10,10,0);
    extras_result_=Control(L"STATIC",L"",SS_LEFT,0,0,10,10,0);
    UiTheme::SetRole(extras_section_,UiTheme::Role::Section); UiTheme::SetRole(extras_result_,UiTheme::Role::Hint);
    apply_=Control(L"BUTTON",L"应用设置",BS_PUSHBUTTON|WS_TABSTOP,0,0,10,10,kApply);
    trigger_=Control(L"BUTTON",L"请求一次投技",BS_PUSHBUTTON|WS_TABSTOP,0,0,10,10,kTrigger);
    cancel_=Control(L"BUTTON",L"取消 / 清理",BS_PUSHBUTTON|WS_TABSTOP,0,0,10,10,kCancel);
    status_section_=Control(L"STATIC",L"运行状态",SS_LEFT,0,0,10,10,0);
    status_=Control(L"EDIT",L"",ES_MULTILINE|ES_READONLY|WS_VSCROLL,0,0,10,10,0);
    detail_section_=Control(L"STATIC",L"操作说明",SS_LEFT,0,0,10,10,0);
    detail_=Control(L"EDIT",L"",ES_MULTILINE|ES_READONLY|WS_VSCROLL,0,0,10,10,0);
    UiTheme::SetRole(status_section_,UiTheme::Role::Section); UiTheme::SetRole(detail_section_,UiTheme::Role::Section);
    UiTheme::SetRole(apply_,UiTheme::Role::Primary);
    Layout(); Refresh(true); return true;
}
}
void Initialize(MotianGrab::Logger logger) { logger_=logger; initialized_=true; }
HWND CreatePage(HWND parent) {
    if (!initialized_||!parent||!Create()) return nullptr;
    DebugHub::Attach(window_,parent,735,640); Layout(); Refresh(false); return window_;
}
void ToggleWindow() {
    if (!initialized_) return;
    if (DebugHub::Enabled()) { DebugHub::ShowPage(DebugHub::Page::MotianGrab); return; }
    if (!Create()) { if (logger_) logger_("MOTIAN_GRAB_MENU Win32 window creation failed"); return; }
    if (IsWindowVisible(window_)) ShowWindow(window_,SW_HIDE);
    else { Refresh(true); ShowWindow(window_,SW_SHOWNORMAL); SetForegroundWindow(window_); SetFocus(enabled_); }
}
void Tick() {
    if (!initialized_ || !window_) return; MSG message{};
    while (!DebugHub::IsEmbedded(window_)&&PeekMessageW(&message,window_,0,0,PM_REMOVE)) if (!IsDialogMessageW(window_,&message)) { TranslateMessage(&message); DispatchMessageW(&message); }
    if (window_ && IsWindowVisible(window_) && GetTickCount64()>=next_refresh_) { next_refresh_=GetTickCount64()+250; Refresh(false); }
}
void Shutdown() { if (window_) DestroyWindow(window_); font_=nullptr; initialized_=false; }
}
