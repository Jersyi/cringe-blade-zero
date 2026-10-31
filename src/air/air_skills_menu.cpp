#include "air_skills_menu.h"
#include "debug_hub.h"
#include "ui_i18n.h"
#include "ui_layout.h"
#include "ui_theme.h"
#include <windows.h>
#include <algorithm>
#include <cmath>
#include <cwchar>
#include <iterator>
#include <string>

namespace AirSkillsMenu {
namespace {
constexpr wchar_t kClass[]=L"PBZ_AirSkills_JC_Window_v1";
constexpr int kAir=101,kStep=102,kSpeed=103,kRadius=104,kApply=105,kOff=106,kSlow=107;
HWND window_=nullptr,air_=nullptr,step_=nullptr,slow_=nullptr,speed_=nullptr,radius_=nullptr,status_=nullptr,jc_gate_=nullptr,summary_=nullptr,skills_=nullptr;
HWND reward_group_=nullptr,intro_=nullptr,switches_=nullptr,parameters_=nullptr,live_=nullptr,skills_label_=nullptr;
HWND speed_label_=nullptr,radius_label_=nullptr,apply_=nullptr,off_=nullptr;
struct RewardControl { const wchar_t* text; bool AirSkills::Options::*option; HWND window=nullptr; };
RewardControl rewards_[]={
    {L"普通杀气回满",&AirSkills::Options::reward_shachi},
    {L"精纯杀气 +1（当前上限内）",&AirSkills::Options::reward_pure_shachi},
    {L"意识之影 +1（当前上限内）",&AirSkills::Options::reward_spirit_shadow},
    {L"技能 CD / 充能一起恢复",&AirSkills::Options::reward_skills},
    {L"重置武器切换 CD",&AirSkills::Options::reward_weapon_switch},
    {L"重置 L1 防御闪避 CD",&AirSkills::Options::reward_guard_dodge},
};
HFONT font_=nullptr;
AirSkills::Logger logger_=nullptr;
ULONGLONG next_refresh_=0;

HWND Control(const wchar_t* cls,const wchar_t* text,DWORD style,int x,int y,int w,int h,int id=0) {
    HWND control=CreateWindowExW(!_wcsicmp(cls,L"EDIT")||!_wcsicmp(cls,L"LISTBOX")?WS_EX_CLIENTEDGE:0,
        cls,text,WS_CHILD|WS_VISIBLE|style,x,y,w,h,window_,reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),GetModuleHandleW(nullptr),nullptr);
    if (control&&font_) SendMessageW(control,WM_SETFONT,reinterpret_cast<WPARAM>(font_),TRUE);
    if (control&&(_wcsicmp(cls,L"EDIT")||(style&ES_READONLY))) UiI18n::Bind(control,text);
    return control;
}
void Populate() {
    const auto state=AirSkills::GetSnapshot();
    SendMessageW(air_,BM_SETCHECK,state.options.air_skills?BST_CHECKED:BST_UNCHECKED,0);
    SendMessageW(step_,BM_SETCHECK,state.options.stomp_jump?BST_CHECKED:BST_UNCHECKED,0);
    SendMessageW(slow_,BM_SETCHECK,state.options.slow_fall?BST_CHECKED:BST_UNCHECKED,0);
    for (const auto& reward:rewards_) SendMessageW(reward.window,BM_SETCHECK,state.options.*(reward.option)?BST_CHECKED:BST_UNCHECKED,0);
    wchar_t text[80]{}; swprintf_s(text,L"%.0f",state.options.bounce_speed); SetWindowTextW(speed_,text);
    swprintf_s(text,L"%.0f",state.options.enemy_radius); SetWindowTextW(radius_,text);
}
bool Number(HWND edit,float& value) {
    wchar_t text[80]{}; GetWindowTextW(edit,text,static_cast<int>(sizeof(text)/sizeof(text[0])));
    wchar_t* end=nullptr; const double parsed=std::wcstod(text,&end);
    while (end&&(*end==L' '||*end==L'\t')) ++end;
    if (!end||end==text||*end||!std::isfinite(parsed)) return false;
    value=static_cast<float>(parsed); return std::isfinite(value);
}
void Message(const wchar_t* source) {
    const std::wstring text=UiI18n::Translate(source),title=UiI18n::Translate(L"空中技能 / JC");
    if (DebugHub::IsEmbedded(window_)&&DebugHub::Notify(text.c_str())) return;
    MessageBoxW(window_,text.c_str(),title.c_str(),MB_OK|MB_ICONINFORMATION);
}
void Submit(bool off) {
    auto options=AirSkills::GetSnapshot().options;
    if (off) options.air_skills=options.stomp_jump=false;
    else {
        options.air_skills=SendMessageW(air_,BM_GETCHECK,0,0)==BST_CHECKED;
        options.stomp_jump=SendMessageW(step_,BM_GETCHECK,0,0)==BST_CHECKED;
        options.slow_fall=SendMessageW(slow_,BM_GETCHECK,0,0)==BST_CHECKED;
        for (const auto& reward:rewards_) options.*(reward.option)=SendMessageW(reward.window,BM_GETCHECK,0,0)==BST_CHECKED;
        if (!Number(speed_,options.bounce_speed)||!Number(radius_,options.enemy_radius)) {
            Message(L"请输入有效数值：弹跳速度 100–3000 cm/s，近敌范围 10–1500 cm。"); return;
        }
    }
    if (!AirSkills::RequestOptions(options)) {
        Message(L"未接受此请求。请检查数值范围；模块因错误停止时需要先恢复或重新启动。"); return;
    }
    if (off) { SendMessageW(air_,BM_SETCHECK,BST_UNCHECKED,0); SendMessageW(step_,BM_SETCHECK,BST_UNCHECKED,0); }
    UiI18n::SetText(status_,L"请求已提交，等待游戏线程处理和读回。"); next_refresh_=GetTickCount64()+300;
}
void Refresh() {
    const auto state=AirSkills::GetSnapshot();
    UiI18n::SetText(status_,state.status.c_str());
    UiI18n::SetText(jc_gate_,state.jc_status.c_str());
    wchar_t distance[48]{};
    if (state.nearest_enemy_surface>=0) swprintf_s(distance,L"%.0f cm",state.nearest_enemy_surface);
    else wcscpy_s(distance,L"尚未识别/测量");
    wchar_t pure[128]{},shadow[128]{};
    const auto resource=[](wchar_t* text,size_t count,const AirSkills::ResourceReward& value,const wchar_t* label) {
        if (!value.known) swprintf_s(text,count,L"%s：尚无奖励读回",label);
        else swprintf_s(text,count,L"%s %.0f→%.0f / %.0f（+%.0f，%s）",label,value.before,value.after,value.maximum,value.after-value.before,value.verified?L"已核对":L"未通过");
    };
    resource(pure,std::size(pure),state.pure_shachi,L"精纯杀气"); resource(shadow,std::size(shadow),state.spirit_shadow,L"意识之影");
    if (!state.options.reward_pure_shachi) wcscpy_s(pure,L"精纯杀气：奖励关闭");
    if (!state.options.reward_spirit_shadow) wcscpy_s(shadow,L"意识之影：奖励关闭");
    wchar_t text[1200]{};
    swprintf_s(text,L"已应用开关：空中/打断 %s　近敌 JC %s　真实跳跃边沿 %u　JC 已确认 %u\r\n主角：%s　跳跃输入：%s　优先级 %d（%s）　已打断技能 %u\r\n近敌候选 %u/%u；AI/队伍/生命拒绝 %u/%u/%u；最近按跳距离 %s（范围 %.0f cm）\r\n技能奖励[%s]累计CD %u / 充能 %u / 未通过 %u　杀气[%s] %.1f / %.1f；扫描 %u 输入资产/%u 技能项\r\n%s　%s\r\n武器切换CD：%s；上次读回 %u 槽，累计成功 %u / 未通过 %u（不跳过切换动作）\r\n仅本次JC下降 %s　重力 %.3f → %.3f　下降上限 %.0f cm/s；待适配 %zu\r\n输入副本 %u　强引用 %u　恢复：%s",
        state.options.air_skills?L"开":L"关",state.options.stomp_jump?L"开":L"关",state.jump_edges,state.bounces,
        state.ready?L"已就绪":L"等待",state.jump_input_ready?L"已识别":L"未识别",state.jump_priority,state.jump_priority_ready?L"已应用":L"未应用",state.jump_cancels,
        state.stomp_targets,state.scanned_pawns,state.enemy_ai_rejected,state.enemy_team_rejected,state.enemy_health_rejected,distance,state.options.enemy_radius,
        state.options.reward_skills?L"开":L"已关闭",state.cooldown_restores,state.restored_charges,state.failed_restores,state.options.reward_shachi?L"开":L"已关闭",
        state.last_shachi,state.last_shachi_max,state.scanned_input_assets,state.scanned_ability_specs,
        pure,shadow,
        !state.options.reward_weapon_switch?L"奖励关闭":state.last_weapon_switch_verified?L"已归零":L"尚未通过",state.last_weapon_switch_slots,state.weapon_switch_resets,state.weapon_switch_failures,
        state.gravity_control_yielded?L"让给外部技能":(state.slow_fall_active?L"生效":L"等待"),state.base_gravity,state.current_gravity,state.down_speed_cap,state.unadapted_skills.size(),
        state.cloned_bindings,state.strong_references,state.restore_pending?L"尚未完整，禁止旅行":L"无待恢复错误");
    wchar_t guard[220]{};
    const std::wstring guard_format=UiI18n::Translate(L"\r\nL1 防御闪避 CD：%s；自身冷却效果 %d→%d，累计成功 %u / 未通过 %u");
    const std::wstring guard_state=UiI18n::Translate(!state.options.reward_guard_dodge?L"奖励关闭":state.last_guard_dodge_verified?L"已归零":state.guard_dodge_known?L"未通过":L"尚无奖励读回");
    swprintf_s(guard,guard_format.c_str(),guard_state.c_str(),
        state.guard_dodge_effects_before,state.guard_dodge_effects_after,state.guard_dodge_resets,state.guard_dodge_failures);
    UiI18n::SetText(summary_,(std::wstring(text)+guard).c_str());
    const LRESULT selection=SendMessageW(skills_,LB_GETCURSEL,0,0),top=SendMessageW(skills_,LB_GETTOPINDEX,0,0);
    SendMessageW(skills_,WM_SETREDRAW,FALSE,0); SendMessageW(skills_,LB_RESETCONTENT,0,0);
    if (state.skills.empty()) UiI18n::AddString(skills_,LB_ADDSTRING,0,L"当前未识别到战技/杀气技能实例；不开启时不扫描主角。");
    for (const auto& skill:state.skills) {
        wchar_t row[1000]{};
        swprintf_s(row,L"[%s] %s　充能 %d/%d　CD %.3f s　%s",skill.input_action.c_str(),skill.ability.c_str(),skill.charges,skill.maximum_charges,skill.cooldown,skill.restored?L"充能和CD已恢复":L"充能或CD未满");
        UiI18n::AddString(skills_,LB_ADDSTRING,0,row);
    }
    for (const auto& skill:state.unadapted_skills) {
        const std::wstring row=L"[待适配] "+skill; UiI18n::AddString(skills_,LB_ADDSTRING,0,row.c_str());
    }
    for (const auto& asset:state.input_assets) {
        const std::wstring row=L"[已扫描输入] "+asset; UiI18n::AddString(skills_,LB_ADDSTRING,0,row.c_str());
    }
    if (selection!=LB_ERR) SendMessageW(skills_,LB_SETCURSEL,static_cast<WPARAM>(selection),0);
    if (top!=LB_ERR) SendMessageW(skills_,LB_SETTOPINDEX,static_cast<WPARAM>(top),0);
    SendMessageW(skills_,WM_SETREDRAW,TRUE,0); InvalidateRect(skills_,nullptr,TRUE);
}
void Hide() {
    if (DebugHub::IsEmbedded(window_)) DebugHub::Hide();
    else if (window_) ShowWindow(window_,SW_HIDE);
}
void Layout() {
    if (!window_||!skills_) return;
    const UiLayout::Metrics m=UiLayout::For(window_);
    RECT client{}; GetClientRect(window_,&client);
    const int margin=m.Margin(),gap=m.Gap(),x=margin,width=(std::max)(m.S(640),static_cast<int>(client.right)-2*margin);
    int y=margin;
    const int intro=UiLayout::TextHeight(intro_,width);
    UiLayout::Place(intro_,x,y,width,intro); y+=intro+gap;
    UiLayout::Place(switches_,x,y,width,m.Section()); y+=m.Section()+gap/2;
    for (HWND check:{air_,step_,slow_}) { UiLayout::Place(check,x,y,(std::min)(width,UiLayout::CheckWidth(check,m)),m.Check()); y+=m.Check()+gap/2; }
    y+=gap;
    UiLayout::Place(reward_group_,x,y,width,m.Section()); y+=m.Section()+gap/2;
    // Reward toggles in an even grid: three columns when they fit, else two.
    int widest=0; for (const auto& reward:rewards_) widest=(std::max)(widest,UiLayout::CheckWidth(reward.window,m));
    const int columns=width>=3*widest+2*gap?3:(width>=2*widest+gap?2:1),cell=(width-(columns-1)*gap)/columns;
    for (size_t i=0;i<std::size(rewards_);++i) {
        const int column=static_cast<int>(i)%columns,row=static_cast<int>(i)/columns;
        UiLayout::Place(rewards_[i].window,x+column*(cell+gap),y+row*(m.Check()+gap/2),(std::min)(cell,UiLayout::CheckWidth(rewards_[i].window,m)),m.Check());
    }
    y+=((static_cast<int>(std::size(rewards_))+columns-1)/columns)*(m.Check()+gap/2)+gap;
    UiLayout::Place(parameters_,x,y,width,m.Section()); y+=m.Section()+gap/2;
    UiLayout::Flow flow(x,y,width,m.Row(),gap,gap);
    flow.Add(speed_label_,UiLayout::TextWidth(speed_label_),m.Line()); flow.Add(speed_,m.S(100)); flow.Space(gap*2);
    flow.Add(radius_label_,UiLayout::TextWidth(radius_label_),m.Line()); flow.Add(radius_,m.S(100));
    y=UiLayout::ButtonRow({apply_,off_},x,flow.Bottom()+gap,width,m,m.S(150))+gap*2;
    UiLayout::Place(live_,x,y,width,m.Section()); y+=m.Section()+gap/2;
    const int two=2*m.Line()+m.S(14);
    UiLayout::Place(status_,x,y,width,two); y+=two+gap;
    UiLayout::Place(jc_gate_,x,y,width,two); y+=two+gap;
    // The summary keeps eight lines; the skill list takes the remaining height.
    const int summary=8*m.Line()+m.S(14),minimum_skills=m.S(170);
    UiLayout::Place(summary_,x,y,width,summary); y+=summary+gap*2;
    UiLayout::Place(skills_label_,x,y,width,m.Section()); y+=m.Section()+gap/2;
    const int skills=(std::max)(minimum_skills,static_cast<int>(client.bottom)-margin-y);
    UiLayout::Place(skills_,x,y,width,skills);
    SendMessageW(skills_,LB_SETHORIZONTALEXTENT,m.S(2200),0);
    DebugHub::SetMinimum(window_,m.S(640)+2*margin,y+minimum_skills+margin);
}
LRESULT CALLBACK WindowProc(HWND window,UINT message,WPARAM wparam,LPARAM lparam) {
    if (message==WM_CLOSE) { Hide(); return 0; }
    if (message==WM_SIZE) { Layout(); return 0; }
    if (message==WM_DPICHANGED_AFTERPARENT) { font_=UiTheme::GetFont(UiTheme::Font::Body,window); UiLayout::UseFont(window,font_); Layout(); return 0; }
    if (message==WM_COMMAND) {
        if (LOWORD(wparam)==IDCANCEL) { Hide(); return 0; }
        if (LOWORD(wparam)==kApply&&HIWORD(wparam)==BN_CLICKED) { Submit(false); return 0; }
        if (LOWORD(wparam)==kOff&&HIWORD(wparam)==BN_CLICKED) { Submit(true); return 0; }
    }
    return DefWindowProcW(window,message,wparam,lparam);
}
bool Create() {
    if (window_&&IsWindow(window_)) return true;
    WNDCLASSW cls{}; cls.lpfnWndProc=WindowProc; cls.hInstance=GetModuleHandleW(nullptr); cls.lpszClassName=kClass;
    cls.hCursor=LoadCursorW(nullptr,IDC_ARROW); cls.hbrBackground=reinterpret_cast<HBRUSH>(COLOR_BTNFACE+1);
    if (!RegisterClassW(&cls)&&GetLastError()!=ERROR_CLASS_ALREADY_EXISTS) return false;
    window_=CreateWindowExW(0,kClass,L"PBZ 空中技能 / 跳跃优先 / 近敌 JC",WS_OVERLAPPED|WS_CAPTION|WS_SYSMENU|WS_MINIMIZEBOX,
        CW_USEDEFAULT,CW_USEDEFAULT,880,880,nullptr,nullptr,cls.hInstance,nullptr);
    if (!window_) return false;
    UiI18n::Bind(window_,L"PBZ 空中技能 / 跳跃优先 / 近敌 JC");
    font_=UiTheme::GetFont(UiTheme::Font::Body,window_);
    intro_=Control(L"STATIC",L"两项独立控制，跟随游戏当前跳跃改键/手柄绑定；菜单、死亡和剧情中不触发。\r\nJC 奖励可在下方分别选择，默认全部开启；只在确认实际弹起后发放。",SS_LEFT,0,0,10,10);
    switches_=Control(L"STATIC",L"功能开关",SS_LEFT,0,0,10,10);
    air_=Control(L"BUTTON",L"空中释放全部技能 + 跳跃优先打断并起跳（保留原生空中轻重攻击）",BS_AUTOCHECKBOX|WS_TABSTOP,0,0,10,10,kAir);
    step_=Control(L"BUTTON",L"开启近敌 JC 弹跳（动作与下方奖励选项独立）",BS_AUTOCHECKBOX|WS_TABSTOP,0,0,10,10,kStep);
    slow_=Control(L"BUTTON",L"仅本次 JC 腾空下降减速25%（重力/终端上限 ×0.75；落地/下一跳恢复）",BS_AUTOCHECKBOX|WS_TABSTOP,0,0,10,10,kSlow);
    reward_group_=Control(L"STATIC",L"JC 奖励（默认全部开启；取消勾选后不执行对应恢复）",SS_LEFT,0,0,10,10);
    for (size_t i=0;i<std::size(rewards_);++i) rewards_[i].window=Control(L"BUTTON",rewards_[i].text,BS_AUTOCHECKBOX|WS_TABSTOP,0,0,10,10,201+static_cast<int>(i));
    parameters_=Control(L"STATIC",L"参数",SS_LEFT,0,0,10,10);
    speed_label_=Control(L"STATIC",L"弹跳速度（cm/s，100–3000）",SS_LEFT,0,0,10,10);
    speed_=Control(L"EDIT",L"750",ES_AUTOHSCROLL|WS_TABSTOP,0,0,10,10,kSpeed);
    radius_label_=Control(L"STATIC",L"近敌范围（cm，10–1500）",SS_LEFT,0,0,10,10);
    radius_=Control(L"EDIT",L"250",ES_AUTOHSCROLL|WS_TABSTOP,0,0,10,10,kRadius);
    apply_=Control(L"BUTTON",L"应用",BS_PUSHBUTTON|WS_TABSTOP,0,0,10,10,kApply);
    off_=Control(L"BUTTON",L"关闭功能 / 恢复原限制",BS_PUSHBUTTON|WS_TABSTOP,0,0,10,10,kOff);
    live_=Control(L"STATIC",L"运行状态",SS_LEFT,0,0,10,10);
    status_=Control(L"EDIT",L"等待状态",ES_MULTILINE|ES_READONLY|WS_VSCROLL,0,0,10,10);
    jc_gate_=Control(L"EDIT",L"近敌 JC 当前关闭",ES_MULTILINE|ES_READONLY|WS_VSCROLL,0,0,10,10);
    summary_=Control(L"EDIT",L"",ES_MULTILINE|ES_READONLY|WS_VSCROLL,0,0,10,10);
    skills_label_=Control(L"STATIC",L"当前战技 / 杀气技能（L2/R2 对应动作，实际 InputAction 随游戏映射）",SS_LEFT,0,0,10,10);
    skills_=Control(L"LISTBOX",L"",LBS_NOINTEGRALHEIGHT|WS_VSCROLL|WS_HSCROLL|WS_TABSTOP,0,0,10,10);
    UiTheme::SetRole(intro_,UiTheme::Role::Hint);
    for (HWND section:{switches_,reward_group_,parameters_,live_,skills_label_}) UiTheme::SetRole(section,UiTheme::Role::Section);
    UiTheme::SetRole(apply_,UiTheme::Role::Primary);
    Layout(); Populate(); Refresh(); return true;
}
}
void Initialize(AirSkills::Logger logger) { logger_=logger; }
HWND CreatePage(HWND parent) {
    if (!parent||!Create()) return nullptr;
    DebugHub::Attach(window_,parent,820,700); Layout(); Refresh(); return window_;
}
void ToggleWindow() {
    if (DebugHub::Enabled()) { DebugHub::ShowPage(DebugHub::Page::AirSkills); return; }
    if (!Create()) { if (logger_) logger_("AIR_MENU native window creation failed"); return; }
    if (IsWindowVisible(window_)) ShowWindow(window_,SW_HIDE);
    else { Populate(); Refresh(); ShowWindow(window_,SW_SHOW); SetForegroundWindow(window_); }
}
void Tick() {
    if (!window_||!IsWindow(window_)) return;
    MSG message{};
    while (!DebugHub::IsEmbedded(window_)&&PeekMessageW(&message,window_,0,0,PM_REMOVE)) {
        if (message.message==WM_KEYDOWN&&message.wParam==VK_ESCAPE) { Hide(); continue; }
        if (!IsDialogMessageW(window_,&message)) { TranslateMessage(&message); DispatchMessageW(&message); }
    }
    if (IsWindowVisible(window_)&&GetTickCount64()>=next_refresh_) { next_refresh_=GetTickCount64()+300; Refresh(); }
}
}
