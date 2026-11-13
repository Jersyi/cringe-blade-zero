#pragma once
// 身材（骨骼）改写：读的是引擎每帧算完的 component-space 姿势 buffer，写也写那一份。
// 和 UUEPBS 做同一件事，但只有这一份代码动这个 buffer，所以不会和别的模组抢同一个函数指针。
#include <cstdint>

namespace NpcBody
{
    // UE5 FTransform：旋转 / 位移 / 缩放，各 4 个 double，共 96 字节。
    struct alignas(16) Xform
    {
        double rot[4];  // +0x00 Rotation
        double pos[4];  // +0x20 Translation
        double scl[4];  // +0x40 Scale3D（第四位不用）
    };
    static_assert(sizeof(Xform) == 96, "UE5 FTransform is 96 bytes");

    // 每帧调一次（传主角的 Pawn->Mesh）。第一次会按属性名问引擎要 buffer 的偏移，
    // 问不到才用 pin 的值。返回这一帧是否读到了 buffer。
    bool Tick(void* component);

    bool Ready();
    int32_t Count();       // 这一帧读到的骨数
    const char* Report();  // 一行状态，调用方直接进日志

    // 调试：读到的前 count 根骨的样子（下标、缩放、位移长度），返回一段文字。
    const char* Dump(int32_t count);

    // 按名字找骨骼下标（大小写不敏感，一次遍历把 countNames 个候选全问完）。
    int32_t FindBones(void* component, const char* const* names, int32_t countNames, int32_t* out);

    // 骨架里的骨骼名（面板要显示）。第一次问过就缓存，换组件或骨数变了才重问。
    int32_t Names(void* component);
    const wchar_t* NameAt(int32_t index);
    int32_t SnapshotNames(wchar_t* out, int32_t slotChars, int32_t maxSlots);
    int32_t SnapshotCount();

    // 父子表（原版也是靠这个）：GetParentBone 逐根问一次，之后缓存。
    // 没有它就做不了「子骨保持粗细」和「Chain 组只留最顶骨」。
    int32_t Parents(void* component);
    int32_t ParentOf(int32_t bone);

    // ---- 详细页：一根骨一个编辑 ----
    // axis：0=x 1=y 2=z 3=整体；slot < 0 表示自动追加。同一 slot 反复调用只改它的参数。
    void SetEdit(int32_t slot, int32_t bone, int32_t axis, double factor);
    void ClearEdits();
    int32_t EditCount();
    void SetStretch(int32_t bone, double factor);

    // ---- 简化页：一个身体部位一栏（照抄 UUEPBS 的 26 组）----
    // 一栏是一组骨（大腿 = 主骨 + 扭转骨 + 矫正骨），并带一个「子骨怎么跟」的模式。
    struct GroupInfo
    {
        wchar_t title[48];   // "Thighs"
        wchar_t note[64];    // "大腿的粗细"
        wchar_t spread[32];  // "子骨保持粗细"
        int32_t bones;       // 这组命中了几根骨
        int32_t factor_milli;
    };

    int32_t GroupCount();
    // 游戏线程调一次（换外观后骨数变了要再调），按 BoneDictionary 规则把骨分到各部位。
    int32_t ResolveGroups(void* component);
    // 面板线程调：只写请求，游戏线程下一帧取走并应用。
    int32_t SnapshotGroups(GroupInfo* out, int32_t maxGroups);
    void SetGroup(int32_t group, double factor);
    double GroupFactor(int32_t group);

    // ---- 移动状态（照抄 v1.3 的 Locomotion：4 档，按速度自动切）----
    // 0 idle / 1 walk / 2 run / 3 sprint。状态层叠在滑条之上：缩放相乘，滑条本身不动，
    // 所以同一个部位可以「站着瘦、跑起来粗」。面板写某一档的值，游戏线程按当前速度选档。
    static const int32_t kLocoStates = 4;
    void SetLocoSpeed(double cmps);   // 游戏线程每帧喂一次速度（cm/s）
    int32_t LocoState();              // 现在在哪一档
    void SetLocoDriving(bool on);     // 状态层总开关
    bool LocoDriving();
    void SetLocoFactor(int32_t state, int32_t group, double factor);
    double LocoFactor(int32_t state, int32_t group);
    // 速度阈值（照抄 config.lua 的 LocomotionIdleSpeed/WalkSpeed/RunSpeed）
    static constexpr double kLocoIdle = 30.0;
    static constexpr double kLocoWalk = 300.0;
    static constexpr double kLocoRun = 700.0;

    // ---- 预设：和原版 UUEPBS 同一个目录、同一种 JSON ----
    // 面板线程调 RequestPreset（1 = 存, 2 = 载, 3 = 删），游戏线程在 Tick 里执行，
    // 结果写进 PresetMessage()。载入的那一份作为基底，滑块与部位盖在它上面。
    void RequestPreset(int32_t kind, const char* name);
    const char* PresetMessage();
    // 清掉预设 + 所有滑块 + 所有部位，回到刚进游戏的样子。
    void ResetAll();

    // 改写时机（关键）：只在组件 tick 之后写没用 —— 引擎下一帧的姿势 finalize 会把整份数组重算，
    // 我们的值被抹平。所以把 finalize 那个虚函数换成我们的 detour，先让引擎算完，再在原函数返回之后改这一份。
    // 只跟踪一个组件；别的骨骼网格进来直接透传。装不上返回 false，原因写在 HookReport()。
    // 2026-11-13 起经 BoneMotionRuntime 的钩子（和「骨骼变换」节点共用）：Hook 只是请求，下一帧装上；
    // Hooked() 是请求了且已经装上。HookedComponent() 是请求的那个组件。
    bool Hook(void* component);
    void Unhook();
    bool Hooked();
    void* HookedComponent();
    int32_t HookSlot();      // 打的是哪个 vtable 槽（UUEPBS 在 PBZ 上实测是 380）
    long long HookCalls();   // detour 命中我们这个组件的次数：0 就说明槽不对
    long long HookForeign();  // 透传了别的组件的次数
    const char* HookReport();

    // 姿势 buffer 的两个偏移（界面层要拿它填 PoseLayout）。没解析出来时返回 0。
    int32_t BufferOffset();
    int32_t ReadIndexOffset();
}
