/*
 * pbz_combo_api.h — 豪之刃 MOD「招式派生」插件接口（C ABI）
 *
 * 插件是一个 Windows x64 DLL，放进 version.dll 同目录的 PBZPlugins\ 文件夹，并在「招式派生」页顶栏打开
 * 「派生插件」后由 MOD 载入。插件导出一个入口函数：
 *
 *     PBZ_PLUGIN_EXPORT PbzResult PBZ_CALL PbzPluginMain(const PbzApi* api, PbzPlugin* self);
 *
 * 入口里调用 api->describe 填写插件信息，再按需 set_callbacks / register_signal / register_action。
 * 完整说明见 PLUGIN_API.md。本头文件只用 C 语言类型，任何能生成 x64 DLL 的 C / C++ 编译器都能用。
 *
 * 兼容规则：
 *   - PBZ_COMBO_API_VERSION 只在不兼容的改动时增加（目前一直是 1）；新功能只追加在 PbzApi 末尾，用 PBZ_API_HAS
 *     判断函数是否存在。功能是否存在（不对应单独函数的，例如节点类型）用 api_revision 和 has_feature 判断。
 *   - 每个结构体第一个字段是 size：调用方填 sizeof(结构体)，MOD 只读写这个大小以内的字段。
 *   - 字符串一律 UTF-8，以 0 结尾。MOD 返回的字符串指针只在本次回调 / 本次调用内有效，需要保留请自行复制。
 *
 * 线程：所有回调都在游戏线程上调用，回调里请尽快返回（不要等待、不要做文件或网络 I/O）。
 *       标注「任意线程」的函数可以在任何线程调用（会排队到下一帧执行）；其余函数只能在游戏线程
 *       （即回调里）调用，否则返回 PBZ_E_THREAD。
 */
#ifndef PBZ_COMBO_API_H
#define PBZ_COMBO_API_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PBZ_COMBO_API_VERSION 1
/* 接口修订号：单调递增，只增不减；这是本头文件对应的修订。MOD 的 api->api_revision 是它实际支持的修订
 * （api->size 里没有这个字段的旧版 MOD 是修订 1）。每次追加函数、事件、节点类型或字段都加 1。
 *   1  2026-11-12  派生插件接口首版（没有 api_revision 字段）
 *   2  2026-11-13  api_revision / has_feature、插件权限（describe_ex、PBZ_E_DENIED）、模块注册表（属性、动作、
 *                  事件）、命令总线、插件 UI 页；PBZ_NODE_HERO_SIZE 也在这一天加入（has_feature("hero_size_node")）
 *   3  2026-11-13  Lua 脚本（PBZPlugins\scripts\*.lua，全局表 pbz，和本接口一一对应）；C 接口没有新函数，只新增
 *                  has_feature("lua_scripts")，宿主的 host 模块新增 scripts_loaded / reload_scripts */
#define PBZ_API_REVISION 3
#define PBZ_CALL __cdecl
#ifdef __cplusplus
#define PBZ_PLUGIN_EXPORT extern "C" __declspec(dllexport)
#else
#define PBZ_PLUGIN_EXPORT __declspec(dllexport)
#endif

/* ---- 结果码 ------------------------------------------------------------------------------------------- */
typedef int32_t PbzResult;
enum {
    PBZ_OK = 0,
    PBZ_E_ARG = -1,          /* 参数为空、越界或格式不对 */
    PBZ_E_STATE = -2,        /* 当前状态做不了（例如没有可用的派生链、主角不可用） */
    PBZ_E_THREAD = -3,       /* 只能在游戏线程（回调里）调用 */
    PBZ_E_NOT_FOUND = -4,    /* 节点、派生表、信号或动作不存在 */
    PBZ_E_FULL = -5,         /* 数量达到上限 */
    PBZ_E_EXISTS = -6,       /* 同名已注册，或与其他信号的编码冲突 */
    PBZ_E_BUFFER = -7,       /* 缓冲区太小（needed 给出所需字节数，含结尾 0） */
    PBZ_E_DISABLED = -8,     /* 派生插件已在页面上关闭 */
    PBZ_E_VERSION = -9,      /* 插件要求的接口版本（或修订）高于本 MOD */
    PBZ_E_DENIED = -10       /* 玩家没有给这个插件授权（见 PBZ_CAP_*）；修订 2 起 */
};

/* ---- 权限（修订 2）---------------------------------------------------------------------------------------------
 * 插件在 describe_ex 里声明需要的能力，玩家在「派生插件」页逐个插件打开；没打开的能力，对应的调用返回 PBZ_E_DENIED
 * （并在日志里记一行）。旧的 describe 等于只要 PBZ_CAP_COMBO。权限随时可以被玩家收回，立即生效。
 * 注意：权限管的是「通过这套接口能做什么」，不是沙箱——插件本身是游戏进程里的原生代码。 */
enum {
    PBZ_CAP_COMBO = 1u << 0,    /* 派生：节点、信号、事件、控制派生链、编辑派生表、派生页选项，写 [NpcCombo] 和 [Plugin.<id>] 两段设置。总是授予 */
    PBZ_CAP_READ = 1u << 1,     /* 读模块属性（prop_get）、订阅模块事件（module_subscribe） */
    PBZ_CAP_WRITE = 1u << 2,    /* 写模块属性（prop_set）、调用模块动作（action_invoke）、UI 页上绑定了属性的控件 */
    PBZ_CAP_INI = 1u << 3,      /* 写 PBZDebugMenu.ini 里除上面两段和 [Plugins] 之外的任意一项（set_setting） */
    PBZ_CAP_COMMAND = 1u << 4,  /* 命令总线（command） */
    PBZ_CAP_UI = 1u << 5,       /* 声明 UI 页（ui_page） */
    PBZ_CAP_UE = 1u << 6,       /* 预留给虚幻引擎反射；评估后没有实现，修订 2 里没有函数用它（见 PLUGIN_API.md「引擎反射」） */
    PBZ_CAP_ALL = 0x7Fu
};

/* action_invoke / command 的 flags */
enum { PBZ_CALL_LATER = 1 };  /* 任意线程可用：排队执行（模块 / 命令属于哪个线程就排到哪个线程），不返回文本；
                               * 结果以模块事件 host / command_done（命令）或 action_done（动作）回给调用的插件 */

/* ---- 按键（= MOD 的 Trigger 值，保存在 .combo 文件里，永不改变） --------------------------------------- */
enum {
    PBZ_BUTTON_NPC_KEY = 0,           /* NPC 技能键 */
    PBZ_BUTTON_LIGHT = 1,             /* 主角轻攻击 */
    PBZ_BUTTON_HEAVY = 2,             /* 主角重攻击 */
    PBZ_BUTTON_SHACHI = 3,            /* 主角影之武装 */
    PBZ_BUTTON_SPECIAL = 4,           /* 主角特殊攻击 */
    PBZ_BUTTON_COMBAT_ART = 5,        /* 主角战技 */
    PBZ_BUTTON_JUMP = 6,              /* 主角跳跃 */
    PBZ_BUTTON_DASH = 7,              /* 主角闪避 */
    PBZ_BUTTON_CONTINUOUS_DASH = 8,   /* 主角连续闪避 */
    PBZ_BUTTON_PERFECT_DODGE = 9,     /* 主角完美闪避 */
    PBZ_BUTTON_DEFLECT_DASH = 10,     /* 主角偏斜闪避 */
    PBZ_BUTTON_SPECIAL_DASH = 11,     /* 主角特殊闪避 */
    PBZ_BUTTON_ANY_DODGE = 12,        /* 主角任意闪避（只用于匹配，press 时请用具体的闪避） */
    PBZ_BUTTON_EXECUTION = 13,        /* 主角处决 */
    PBZ_BUTTON_COMBO_KEYS = 14,       /* 自定义组合键（节点类型，不能 press） */
    PBZ_BUTTON_KEYS = 15,             /* 自定义按键（节点类型，不能 press；插件信号走这条路） */
    PBZ_BUTTON_STANCE = 16,           /* 主角格挡 */
    PBZ_BUTTON_SIMPLE_EXECUTION = 17, /* 主角简易处决 */
    PBZ_BUTTON_COUNT = 18
};

/* ---- 节点类型（= MOD 的 NodeType 值，保存在 .combo 文件里，永不改变） ------------------------------------ */
enum {
    PBZ_NODE_SKILL = 0,          /* 当前外观招式 */
    PBZ_NODE_INTERVAL = 1,       /* 间隔 */
    PBZ_NODE_TRIGGER = 2,        /* 按键 / 替换按键 / 自定义组合键 / 自定义按键（起端） */
    PBZ_NODE_AUTO_DODGE = 3,     /* 方向闪避 */
    PBZ_NODE_BRANCH = 4,         /* 按键分支 */
    PBZ_NODE_SPEED = 5,          /* 加速 */
    PBZ_NODE_WEAPON = 6,         /* 武器属性 */
    PBZ_NODE_ARMOR = 7,          /* 霸体 */
    PBZ_NODE_INTERRUPT = 8,      /* 允许中断 */
    PBZ_NODE_GRAB = 9,           /* 投技 */
    PBZ_NODE_BOSS_SKILL = 10,    /* BOSS 技能 */
    PBZ_NODE_WEAPON_MOVE = 11,   /* 武器派生 */
    PBZ_NODE_DAMAGE = 12,        /* 伤害倍率 */
    PBZ_NODE_GROUP = 13,         /* 组合 */
    PBZ_NODE_TELEPORT = 14,      /* 瞬移 */
    PBZ_NODE_INVINCIBLE = 15,    /* 无敌 */
    PBZ_NODE_LOOK = 16,          /* 变身 */
    PBZ_NODE_HERO_LOOK = 17,     /* 变回主角 */
    PBZ_NODE_KEEP_LOOK = 18,     /* 保留变身 */
    PBZ_NODE_FORCE_HIT = 19,     /* 强制受击 */
    PBZ_NODE_PRE_INPUT = 20,     /* 预输入 */
    PBZ_NODE_WEAPON_ACTIONS = 21,/* 武器派生修改 / 空中派生 */
    PBZ_NODE_IGNORE_GUARD = 22,  /* 无视防御 */
    PBZ_NODE_KEEP_CHAIN = 23,    /* 保持派生 */
    PBZ_NODE_ASSIST = 24,        /* BOSS 援护 */
    PBZ_NODE_HOOK = 25,          /* 钩子 */
    PBZ_NODE_TRIM = 26,          /* 裁剪 */
    PBZ_NODE_AIR = 27,           /* 滞空 */
    PBZ_NODE_AIR_DEFAULT = 28,   /* 空中释放 */
    PBZ_NODE_BONE = 29,          /* 骨骼 */
    PBZ_NODE_BOSS_EFFECT = 30,   /* BOSS 特效 */
    PBZ_NODE_PLUGIN = 31,        /* 插件节点（本接口新增） */
    PBZ_NODE_HERO_SIZE = 32      /* 主角体型（2026-11-13 新增；旧版 MOD 的 edit_add_node 返回 PBZ_E_ARG；用 has_feature("hero_size_node") 判断） */
};

/* 派生链所处阶段 */
enum { PBZ_PHASE_IDLE = 0, PBZ_PHASE_STARTING = 1, PBZ_PHASE_RUNNING = 2 };

/* 插件节点的两种类型 */
enum {
    PBZ_ACTION_RUN = 0,      /* 动作：派生链走到这里时调用 run，像一招一样「播放」，之后等下一次按键 */
    PBZ_ACTION_DECIDE = 1    /* 判断：按键经过这里时调用 decide，返回走哪个出口（像按键分支，但由插件决定） */
};

/* on_hero_ability 的返回值 */
enum { PBZ_PASS = 0, PBZ_CONSUME = 1 };

/* 事件类型（PbzEvent.type） */
enum {
    PBZ_EVENT_CHAIN_START = 1,   /* 一条派生链开始：trigger = 起端节点，button = 按键，text = 来源 */
    PBZ_EVENT_PRESS = 2,         /* 派生链收到一次按键：taken = 是否被接住，text = 来源 */
    PBZ_EVENT_NODE_PLAY = 3,     /* 开始播放一个节点：node、node_type、ability、boss */
    PBZ_EVENT_MOVE_END = 4,      /* 正在播放的招式结束（派生链开始等待下一次按键）：node */
    PBZ_EVENT_CHAIN_RESET = 5,   /* 派生链结束 / 重置：text = 原因（timeout、unmatched、other-trigger、edited …） */
    PBZ_EVENT_LOOK = 6,          /* 外观换好：text = 现在的外观 id（hero 为主角） */
    PBZ_EVENT_TABLES = 7         /* 派生表被编辑或重新读取（节点序号可能变了，缓存的节点请重新读） */
};

typedef struct PbzPlugin PbzPlugin;  /* MOD 内部的插件句柄，插件只传回给 API */
typedef int32_t PbzEdit;             /* 派生表编辑会话（edit_begin 给出，> 0） */

/* ---- 结构体 -------------------------------------------------------------------------------------------- */

/* 插件信息（describe 用）。字符串由插件持有，至少在插件卸载前有效（通常用字符串常量）。 */
typedef struct PbzPluginInfo {
    uint32_t size;
    const char* id;            /* 唯一 id：字母、数字、_ . -，1~48 字节，例如 "my.conditions" */
    const char* name;          /* 显示名（中文可） */
    const char* version;       /* 例如 "1.0.0" */
    const char* author;
    const char* description;
    uint32_t min_api;          /* 需要的最低 PBZ_COMBO_API_VERSION */
} PbzPluginInfo;

/* 带权限声明的插件信息（describe_ex 用，修订 2）。前 7 个字段与 PbzPluginInfo 相同。 */
typedef struct PbzPluginInfoEx {
    uint32_t size;
    const char* id;
    const char* name;
    const char* version;
    const char* author;
    const char* description;
    uint32_t min_api;
    uint32_t min_revision;     /* 需要的最低 api_revision（0：不限）；高于本 MOD 的修订 → PBZ_E_VERSION */
    uint32_t caps;             /* 需要的能力（PBZ_CAP_*；PBZ_CAP_COMBO 总是授予，可以不写）。玩家在插件列表里逐项授权 */
    const char* caps_reason;   /* 给玩家看的一句话：为什么需要这些能力（可为 NULL） */
} PbzPluginInfoEx;

/* 派生链运行状态（get_state） */
typedef struct PbzChainState {
    uint32_t size;
    int32_t ready;          /* 1：当前角色的派生链可用（有主角，派生链开关开着） */
    int32_t trigger;        /* 当前派生链的起端节点（-1：没有派生链在进行） */
    int32_t cursor;         /* 派生链所在节点（刚放过的招式、组合或起端） */
    int32_t playing;        /* 正在播放的节点（-1：没有） */
    int32_t queued;         /* 已记住的下一招（-1：没有） */
    int32_t button;         /* 派生链当前的按键（PBZ_BUTTON_*） */
    int32_t phase;          /* PBZ_PHASE_* */
    int32_t look_changing;  /* 1：正在变身 / 变回 */
    uint64_t visit;         /* 每播放一个节点就变（循环回到同一节点也会变） */
    uint64_t generation;    /* 派生表的版本：编辑或重新读取后会变 */
    uint64_t now_ms;        /* GetTickCount64 */
    uint64_t move_started_ms; /* 当前招式开始时间（0：没有） */
    uint64_t reset_at_ms;   /* 等待下一次按键时，超时重置的时间（0：不在等待） */
    char table[64];         /* 正在使用的派生表（"Hero" 为主角，其余为 NPC 外观 id） */
    char look[64];          /* 主角现在的外观 id（"hero" 为主角本身） */
} PbzChainState;

/* 一个节点（get_node）。字段含义随节点类型不同，未用到的为 0 / 空。 */
typedef struct PbzNodeInfo {
    uint32_t size;
    int32_t index;
    int32_t type;           /* PBZ_NODE_* */
    int32_t enabled;        /* 起端是否启用（其余节点为 1） */
    int32_t x, y;           /* 画布位置 */
    int32_t next;           /* 出线指向的节点（-1：没有；按键分支 / 判断节点见 routes） */
    int32_t group;          /* 所在组合节点（-1：没有） */
    int32_t ms;             /* 招式的派生点 / 间隔的毫秒 / 起端的重置时间等 */
    int32_t button;         /* 起端的按键（PBZ_BUTTON_*） */
    int32_t route_count;    /* 行数（按键分支、武器派生修改、判断节点的出口） */
    int32_t key_count;      /* 自定义按键 / 自定义组合键起端的按键数 */
    int32_t keys[6];        /* 键码；插件信号的键码见 register_signal */
    int32_t value;          /* 插件节点的数值 */
    int32_t action_kind;    /* 插件节点：PBZ_ACTION_* */
    char ability[128];      /* 招式 / 技能名 */
    char boss[64];          /* BOSS 技能 / 援护 / 变身的外观 id；BOSS 特效的目录 */
    char weapon[64];        /* 武器 id */
    char action[112];       /* 插件节点的动作 id（"插件id/动作id"） */
} PbzNodeInfo;

/* 一行（按键分支、武器派生修改的行，或判断节点的出口） */
typedef struct PbzRouteInfo {
    uint32_t size;
    int32_t button;         /* PBZ_BUTTON_*（判断节点的出口为 -1） */
    int32_t next;
    int32_t pause_ms;       /* 按键分支的停顿 */
    int32_t hold_ms;        /* 按住 */
    int32_t chord;          /* 自定义按键行：1 同时按，0 依次按 */
    int32_t key_count;
    int32_t keys[6];
} PbzRouteInfo;

/* 主角与锁定目标的情况（get_hero，游戏线程） */
typedef struct PbzHeroFacts {
    uint32_t size;
    int32_t valid;          /* 0：主角不可用（读图、过场等） */
    float health, health_max;
    int32_t airborne;       /* 1：在空中 */
    double x, y, z;         /* 位置（厘米） */
    int32_t has_target;     /* 1：有锁定目标或最近的敌人 */
    int32_t target_locked;  /* 1：目标是锁定的（0：最近的敌人） */
    float target_distance;  /* 到目标的距离（厘米，胶囊中心之间） */
    float target_health, target_health_max;
    char weapon[64];        /* 主武器 id（读不到为空） */
    char look[64];          /* 外观 id（"hero" 为主角本身） */
} PbzHeroFacts;

/* 主角的一个技能刚开始（on_hero_ability） */
typedef struct PbzAbilityEvent {
    uint32_t size;
    const char* name;       /* 技能类名（去掉 _C），例如 "GA_Hun_Young_Air_A" */
    uint32_t bits;          /* 它带的主角按键位（1u << PBZ_BUTTON_*；0：不是主角按键动作） */
    int32_t handle;         /* 技能句柄（cancel_hero_ability 用） */
    int32_t chain_own;      /* 1：派生链自己放的技能（通常应忽略） */
    uint64_t now_ms;
} PbzAbilityEvent;

/* 事件（on_event） */
typedef struct PbzEvent {
    uint32_t size;
    int32_t type;           /* PBZ_EVENT_* */
    uint64_t now_ms;
    int32_t trigger;        /* 派生链起端（-1：没有） */
    int32_t node;           /* 相关节点（-1：没有） */
    int32_t node_type;      /* 相关节点的 PBZ_NODE_*（-1：没有） */
    int32_t button;         /* 相关按键 */
    int32_t taken;          /* PRESS：1 被派生链接住 */
    uint32_t press_bits;    /* PRESS：主角按键位；npc_key 为 1 时是 NPC 技能键 */
    int32_t npc_key;
    const char* text;       /* 来源 / 原因 / 外观 id（可为空字符串，不为 NULL） */
    const char* ability;    /* NODE_PLAY：招式名（可为空字符串） */
    const char* boss;       /* NODE_PLAY：外观 id（可为空字符串） */
} PbzEvent;

/* 插件信号：一个虚拟按键。注册后出现在自定义按键的按键列表里（「插件信号」分组），玩家可以把它用作
 * 自定义按键起端、按键分支的自定义按键行、武器派生修改的自定义按键行，也可以和真实按键组合（V + 信号）。 */
typedef struct PbzSignalDesc {
    uint32_t size;
    const char* id;          /* 插件内唯一：字母、数字、_ . -，1~48 字节；完整 id 为 "插件id/信号id" */
    const char* name;        /* 显示名，例如 "空中轻攻击" */
    const char* group;       /* 列表里的分组名（可为 NULL） */
    const char* description; /* 鼠标提示（可为 NULL） */
} PbzSignalDesc;

/* 插件节点被调用时的上下文
 *
 * decide 在按键经过判断节点时调用。派生链处理一次按键时会多次沿连线查找（先判断这次按键会不会被接住，再真正
 * 走过去），所以同一次按键 decide 可能被问到不止一次：同一帧里相同的节点、按键和数值只调用一次，之后用缓存的
 * 结果。decide 里只做判断，不要产生副作用（要做事请用动作节点、on_event 或 on_tick）。 */
typedef struct PbzNodeCall {
    uint32_t size;
    int32_t node;            /* 节点序号 */
    int32_t trigger;         /* 派生链起端（-1：这次按键会开始一条新的派生链） */
    int32_t button;          /* 派生链此刻的按键（PBZ_BUTTON_*） */
    int32_t value;           /* 节点上设置的数值 */
    uint32_t press_bits;     /* 带来这次经过的按键的主角按键位（1u << PBZ_BUTTON_*；自定义按键为 0） */
    int32_t press_npc_key;   /* 1：这次按键是 NPC 技能键 */
    int32_t output_count;    /* 判断节点的出口数 */
    const char* action;      /* 动作 id（不含插件 id） */
    const char* table;       /* 派生表 */
    uint64_t now_ms;
} PbzNodeCall;

typedef int32_t (PBZ_CALL *PbzDecideFn)(void* user, const PbzNodeCall* call);  /* 返回出口序号 0~output_count-1；-1 不走任何出口 */
typedef void (PBZ_CALL *PbzRunFn)(void* user, const PbzNodeCall* call);

/* 插件节点的动作（register_action）。字符串在插件卸载前须一直有效。 */
typedef struct PbzActionDesc {
    uint32_t size;
    const char* id;            /* 插件内唯一：字母、数字、_ . -，1~48 字节 */
    const char* name;          /* 卡片上的标题，例如 "按血量分支" */
    const char* group;         /* 选择列表里的分组（可为 NULL） */
    const char* description;   /* 卡片说明行（可为 NULL） */
    int32_t kind;              /* PBZ_ACTION_RUN 或 PBZ_ACTION_DECIDE */
    int32_t output_count;      /* DECIDE：出口数 1~6 */
    const char* const* outputs;/* DECIDE：每个出口的名字 */
    int32_t has_value;         /* 1：卡片上有一个数值 */
    const char* value_label;   /* 数值名，例如 "血量低于" */
    const char* value_unit;    /* 单位，例如 "%"、"厘米"（可为 NULL） */
    int32_t value_min, value_max, value_step, value_default;
    PbzDecideFn decide;        /* DECIDE 必填 */
    PbzRunFn run;              /* RUN 必填 */
    void* user;                /* 原样传回 decide / run */
} PbzActionDesc;

/* 武器招式表（weapon_move，供插件列出主角武器的技能名） */
typedef struct PbzWeaponMove {
    uint32_t size;
    char weapon[64];          /* 武器 id */
    char ability[128];        /* 技能名 */
    int32_t kind;             /* 0 轻攻击，1 重攻击，2 处决，3 其他技能 */
    int32_t shaqi;            /* 1：影之武装 */
    char stage[16];           /* 轻重段，例如 "AAB"（空：不在段数里） */
} PbzWeaponMove;

/* 模块事件（on_module_event，修订 2）：模块自己发出的事件，例如外观套用完成、武器切换。要先 module_subscribe。
 * 模块「host」的事件 command_done（命令）和 action_done（排队的动作）不用订阅，只回给发出它的插件：text = "ok" 或 "fail"，TAB，命令行（动作为 "模块.动作 参数"），TAB，输出文本。
 * 事件在模块发出之后的下一帧、在游戏线程上递送。字符串只在回调里有效。 */
typedef struct PbzModuleEvent {
    uint32_t size;
    const char* module;       /* 模块名 */
    const char* event;        /* 事件名 */
    const char* text;         /* 附带的文本（格式见各事件的说明；可为空字符串，不为 NULL） */
    uint64_t now_ms;
} PbzModuleEvent;

/* 插件 UI 页上的一个控件（ui_page，修订 2）。宿主用面板自己的控件绘制，插件不接触窗口。 */
enum {
    PBZ_UI_LABEL = 0,    /* 一行文字（text）；label 是左边的小标题（可为空） */
    PBZ_UI_TOGGLE = 1,   /* 开关：value 0 / 1 */
    PBZ_UI_SLIDER = 2,   /* 滑条：min / max / step / value；text 是单位 */
    PBZ_UI_BUTTON = 3,   /* 按钮：label 是按钮上的字；按下时 on_ui 的 value = 1 */
    PBZ_UI_CHOICE = 4,   /* 选项：value 是序号，text 是选项名（逗号分隔）；min / max / step 不用 */
    PBZ_UI_TEXT = 5      /* 多行文字（只读，text 里的 '\n' 换行；用来显示状态、日志） */
};
typedef struct PbzUiItem {
    uint32_t size;
    int32_t kind;             /* PBZ_UI_* */
    const char* id;           /* 页内唯一：字母、数字、_ . -，1~48 字节 */
    const char* label;
    double min, max, step, value;
    const char* text;
    const char* bind;         /* 可选："模块名.属性名"。控件直接读写这个模块属性（读要 PBZ_CAP_READ，写要 PBZ_CAP_WRITE），
                               * 不再调用 on_ui；属性的类型和范围决定控件的范围（min / max / step 可以不填） */
} PbzUiItem;
typedef struct PbzUiPage {
    uint32_t size;
    const char* id;           /* 插件内唯一：字母、数字、_ . -，1~48 字节 */
    const char* title;        /* 页标题 */
    const PbzUiItem* const* items;  /* item_count 个指针（每个 PbzUiItem 自带 size） */
    int32_t item_count;       /* 1~64 */
} PbzUiPage;
/* 玩家操作了一个没有绑定属性的控件（on_ui）。 */
typedef struct PbzUiEvent {
    uint32_t size;
    const char* page;
    const char* item;
    int32_t kind;             /* PBZ_UI_* */
    double value;             /* TOGGLE 0 / 1，SLIDER、CHOICE 的新值，BUTTON 为 1 */
    uint64_t now_ms;
} PbzUiEvent;

/* 回调（set_callbacks）。不需要的填 NULL。 */
typedef struct PbzCallbacks {
    uint32_t size;
    void* user;                                                            /* 原样传回 */
    void (PBZ_CALL *on_event)(void* user, const PbzEvent* event);
    int32_t (PBZ_CALL *on_hero_ability)(void* user, const PbzAbilityEvent* ability);  /* PBZ_PASS / PBZ_CONSUME */
    void (PBZ_CALL *on_tick)(void* user, uint64_t now_ms);                 /* 每帧一次 */
    void (PBZ_CALL *on_enabled)(void* user, int32_t enabled);              /* 页面上开 / 关派生插件 */
    /* 修订 2 起 */
    void (PBZ_CALL *on_module_event)(void* user, const PbzModuleEvent* event);  /* 订阅的模块事件 */
    void (PBZ_CALL *on_ui)(void* user, const PbzUiEvent* event);                /* UI 页上没有绑定属性的控件被操作 */
    void (PBZ_CALL *on_caps)(void* user, uint32_t granted);                     /* 玩家改了这个插件的授权（granted = PBZ_CAP_* 位） */
} PbzCallbacks;

/* ---- API 函数表 ---------------------------------------------------------------------------------------- */
typedef struct PbzApi {
    uint32_t size;               /* sizeof(PbzApi)（MOD 的）：新函数追加在末尾，用 PBZ_API_HAS 判断 */
    uint32_t version;            /* PBZ_COMBO_API_VERSION（MOD 的） */
    const char* mod_version;     /* MOD 版本，例如 "20261112" */

    /* 插件自身 */
    PbzResult (PBZ_CALL *describe)(PbzPlugin* self, const PbzPluginInfo* info);           /* 入口里必须先调用 */
    PbzResult (PBZ_CALL *set_callbacks)(PbzPlugin* self, const PbzCallbacks* callbacks);
    void (PBZ_CALL *log)(PbzPlugin* self, const char* text);                               /* 任意线程；写入 PBZDebugMenu.log */
    void (PBZ_CALL *notify)(PbzPlugin* self, const char* text);                            /* 任意线程；面板右下角提示 */
    uint64_t (PBZ_CALL *now_ms)(void);                                                     /* 任意线程；GetTickCount64 */
    int32_t (PBZ_CALL *enabled)(void);                                                     /* 任意线程；派生插件开关 */

    /* 插件信号（虚拟按键） */
    PbzResult (PBZ_CALL *register_signal)(PbzPlugin* self, const PbzSignalDesc* desc, int32_t* key_code);
    PbzResult (PBZ_CALL *fire_signal)(PbzPlugin* self, const char* id);                   /* 任意线程；按一下（下一帧按下、再下一帧松开） */
    PbzResult (PBZ_CALL *set_signal)(PbzPlugin* self, const char* id, int32_t down);      /* 任意线程；按住 / 松开 */
    int32_t (PBZ_CALL *signal_wanted)(PbzPlugin* self, const char* id);                   /* 游戏线程；1：派生链此刻有起端或下一行在等这个信号 */

    /* 插件节点 */
    PbzResult (PBZ_CALL *register_action)(PbzPlugin* self, const PbzActionDesc* desc);

    /* 读取派生链（游戏线程） */
    PbzResult (PBZ_CALL *get_state)(PbzChainState* state);
    int32_t (PBZ_CALL *node_count)(void);                                                  /* 正在使用的派生表的节点数 */
    PbzResult (PBZ_CALL *get_node)(int32_t index, PbzNodeInfo* node);
    PbzResult (PBZ_CALL *get_route)(int32_t node, int32_t row, PbzRouteInfo* route);
    PbzResult (PBZ_CALL *get_hero)(PbzHeroFacts* facts);

    /* 控制派生链 */
    PbzResult (PBZ_CALL *press)(PbzPlugin* self, int32_t button);                         /* 任意线程；向派生链送一次按键（不会放出主角自己的动作） */
    PbzResult (PBZ_CALL *reset_chain)(PbzPlugin* self, const char* reason);              /* 任意线程；结束当前派生链 */
    PbzResult (PBZ_CALL *cancel_hero_ability)(int32_t handle);                            /* 游戏线程；取消主角正在放的一个技能 */

    /* 派生表文件（任意线程） */
    PbzResult (PBZ_CALL *list_tables)(char* buffer, int32_t size, int32_t* needed);       /* 当前 .combo 里的派生表 id，用 '\n' 分隔 */
    PbzResult (PBZ_CALL *export_table)(const char* table, char* buffer, int32_t size, int32_t* needed);  /* 派生表的保存文本 */
    PbzResult (PBZ_CALL *import_table)(PbzPlugin* self, const char* table, const char* text);           /* 写入并保存（会覆盖！） */

    /* 武器招式表（任意线程） */
    int32_t (PBZ_CALL *weapon_move_count)(void);
    PbzResult (PBZ_CALL *get_weapon_move)(int32_t index, PbzWeaponMove* move);

    /* 插件自己注册的东西（游戏线程）。同一个 id 再 register 一次就是更新（名字、分组、出口、数值范围、回调都换成
     * 新的，信号的键码不变）；注销后，派生表里用到它的节点和按键原样保留，只是不再起作用，重新注册即恢复。 */
    PbzResult (PBZ_CALL *unregister_signal)(PbzPlugin* self, const char* id);
    PbzResult (PBZ_CALL *unregister_action)(PbzPlugin* self, const char* id);

    /* 直接播放派生链里的一个招式节点（任意线程，下一帧执行）：立即打断正在播放的招式，不等派生点；没有派生链在
     * 进行时从连到这个节点的起端开始一条。button：之后派生链所在的按键（-1：沿用当前的）。 */
    PbzResult (PBZ_CALL *play_node)(PbzPlugin* self, int32_t node, int32_t button);

    /* 派生表编辑会话（任意线程；同一个会话不要同时在两个线程里用）。
     * edit_begin 把一个派生表读进一份副本，之后的 edit_* 都只改这份副本；edit_commit 一次写回 .combo 文件并生效：
     * 正在使用这个派生表的派生链会重置，页面会重新读取。写回前按保存格式整理一遍，不合法的连线、数值会像页面保存
     * 时一样被去掉或改到范围内。edit_cancel 放弃副本。每个插件最多同时开 8 个会话。
     * 字段（edit_get_field / edit_set_field）：node = -1 是派生表本身（字段 "enabled"）；row = -1 是节点的字段，
     * row >= 0 是节点那一行的字段。值一律是文本：整数、0 / 1、字符串，列表用逗号分隔。字段名见 PLUGIN_API.md 或
     * edit_field_names。 */
    PbzResult (PBZ_CALL *edit_begin)(PbzPlugin* self, const char* table, PbzEdit* edit);
    PbzResult (PBZ_CALL *edit_commit)(PbzPlugin* self, PbzEdit edit);
    PbzResult (PBZ_CALL *edit_cancel)(PbzPlugin* self, PbzEdit edit);
    int32_t (PBZ_CALL *edit_node_count)(PbzPlugin* self, PbzEdit edit);
    PbzResult (PBZ_CALL *edit_get_node)(PbzPlugin* self, PbzEdit edit, int32_t index, PbzNodeInfo* node);
    PbzResult (PBZ_CALL *edit_get_route)(PbzPlugin* self, PbzEdit edit, int32_t node, int32_t row, PbzRouteInfo* route);
    PbzResult (PBZ_CALL *edit_add_node)(PbzPlugin* self, PbzEdit edit, int32_t type, int32_t x, int32_t y, int32_t* index);  /* 页面上新建时的默认值 */
    PbzResult (PBZ_CALL *edit_remove_node)(PbzPlugin* self, PbzEdit edit, int32_t index);  /* 连进它的线接到它第一个出口原来连的节点；之后的序号减 1 */
    PbzResult (PBZ_CALL *edit_get_field)(PbzPlugin* self, PbzEdit edit, int32_t node, int32_t row, const char* field, char* buffer, int32_t size, int32_t* needed);
    PbzResult (PBZ_CALL *edit_set_field)(PbzPlugin* self, PbzEdit edit, int32_t node, int32_t row, const char* field, const char* value);
    PbzResult (PBZ_CALL *edit_add_route)(PbzPlugin* self, PbzEdit edit, int32_t node, int32_t* row);     /* 按键分支、武器派生修改、判断节点 */
    PbzResult (PBZ_CALL *edit_remove_route)(PbzPlugin* self, PbzEdit edit, int32_t node, int32_t row);
    PbzResult (PBZ_CALL *edit_link)(PbzPlugin* self, PbzEdit edit, int32_t from, int32_t output, int32_t to);  /* 出线；to = -1 断开 */
    PbzResult (PBZ_CALL *edit_link_modifier)(PbzPlugin* self, PbzEdit edit, int32_t from, int32_t modifier);  /* 箭头指向修饰节点 */
    PbzResult (PBZ_CALL *edit_unlink_modifier)(PbzPlugin* self, PbzEdit edit, int32_t from, int32_t slot);    /* 断开箭头；slot = -1 全部 */
    PbzResult (PBZ_CALL *edit_export)(PbzPlugin* self, PbzEdit edit, char* buffer, int32_t size, int32_t* needed);  /* 副本的保存文本 */
    PbzResult (PBZ_CALL *edit_import)(PbzPlugin* self, PbzEdit edit, const char* text);                          /* 用保存文本替换副本 */
    PbzResult (PBZ_CALL *edit_field_names)(int32_t row_fields, char* buffer, int32_t size, int32_t* needed);     /* 字段名，每行一个（row_fields：0 节点字段，1 行字段） */

    /* MOD 的选项（任意线程，立即生效并保存）：
     *   "skill_damage"      派生页「技能伤害」：0 跟随 NPC 技能页，否则 0.1~10 倍
     *   "grab_damage_mode"  派生页「投技伤害」：-1 跟随投技页，0 原生，1 倍率，2 每段至少最大生命的百分比
     *   "grab_damage_value" 投技伤害的数值（倍率或百分比）
     *   "chain_hud"         派生显示：0 关，1 详细，2 简略
     *   "view_target"       视角跟随盯的骨骼：0 骨盆，1 脖子，2 头部
     *   "show_toast"        派生提示：0 / 1
     *   "plugins"           派生插件开关（只读）
     * 设置文件（任意线程）：PBZDebugMenu.ini 里的任意一项。读的是 MOD 缓存的值；写入立即保存，读取这一项的功能
     * 下次读取时生效（有的在下次启动或重新打开对应页面后）。 */
    PbzResult (PBZ_CALL *get_option)(const char* name, double* value);
    PbzResult (PBZ_CALL *set_option)(PbzPlugin* self, const char* name, double value);
    PbzResult (PBZ_CALL *get_setting)(const char* section, const char* key, char* buffer, int32_t size, int32_t* needed);
    PbzResult (PBZ_CALL *set_setting)(PbzPlugin* self, const char* section, const char* key, const char* value);
    /* set_setting 的权限（修订 2 起）：PBZ_CAP_COMBO 只能写 [NpcCombo] 和 [Plugin.<插件id>]；其他段要 PBZ_CAP_INI，
     * 否则 PBZ_E_DENIED。[Plugins] 仍然只有玩家能改。 */

    /* ===================================== 以下是修订 2（2026-11-13）追加的 ===================================== */

    /* 版本与特性 */
    uint32_t api_revision;       /* MOD 支持的修订（PBZ_API_REVISION 的值）。先用 PBZ_API_HAS(api, api_revision) 判断它在不在 */
    uint32_t reserved0;
    /* 任意线程。1：有这个特性。名字（小写）：
     *   "hero_size_node"  节点类型 PBZ_NODE_HERO_SIZE（修饰槽 mod.hero_size）
     *   "permissions"     describe_ex、granted_caps、PBZ_E_DENIED
     *   "modules"         模块注册表：module_list / prop_* / action_*
     *   "module_events"   module_subscribe 和 on_module_event
     *   "commands"        命令总线 command / command_list
     *   "ui_pages"        UI 页 ui_page 等
     *   "lua_scripts"     这个 MOD 会读 PBZPlugins\scripts\*.lua（且玩家没有关掉 [Plugins] Scripts）；修订 3
     * 未知的名字返回 0（引擎反射没有实现，has_feature("ue") 是 0，原因见 PLUGIN_API.md「引擎反射」）。 */
    int32_t (PBZ_CALL *has_feature)(const char* name);

    /* 带权限声明的 describe（入口里先于其他调用；和 describe 二选一，也可以先后各调一次，id 必须一致）。 */
    PbzResult (PBZ_CALL *describe_ex)(PbzPlugin* self, const PbzPluginInfoEx* info);
    /* 任意线程。玩家现在给这个插件的能力（PBZ_CAP_* 位，总含 PBZ_CAP_COMBO；插件没有 describe 过返回 0）。 */
    uint32_t (PBZ_CALL *granted_caps)(PbzPlugin* self);

    /* ---- 模块注册表（任意线程；列表函数不需要授权）--------------------------------------------------------------
     * MOD 的每个功能模块（外观、身材、毛发、Boss Rush……）注册一份描述表：属性、动作、事件。下面的列表是文本：
     * 一行一项（'\n' 分隔），列用 '\t' 分隔；以后可能在行尾追加列，请忽略多出来的列。文本里的 \t \n 已换成空格。
     * module_list 的列：名字 | 显示名 | 线程 | 属性数 | 动作数 | 事件数 | 说明。
     * 线程列：game = 只在游戏线程上读写；any = 模块自己线程安全；worker = 属于面板线程（读写经模块自己的请求队列，
     *         生效可能晚一两个面板周期）。写入一律排队到游戏线程再调模块（见 prop_set）。 */
    PbzResult (PBZ_CALL *module_list)(char* buffer, int32_t size, int32_t* needed);
    /* prop_list 的列：名字 | 类型(bool/int/float/enum/text) | 读写(rw/ro) | 最小 | 最大 | 默认 | 持久化("段.键"，空：不持久)
     *               | 线程 | 显示名 | 选项(enum 的名字，逗号分隔；序号从 0) | 说明。 */
    PbzResult (PBZ_CALL *prop_list)(const char* module, char* buffer, int32_t size, int32_t* needed);
    /* action_list 的列：名字 | 线程 | 持久(1：会写 ini 或文件，0：只改当前状态) | 参数说明 | 显示名 | 说明。 */
    PbzResult (PBZ_CALL *action_list)(const char* module, char* buffer, int32_t size, int32_t* needed);
    /* event_list 的列：名字 | 显示名 | 附带文本的格式说明。 */
    PbzResult (PBZ_CALL *event_list)(const char* module, char* buffer, int32_t size, int32_t* needed);

    /* 读属性（需要 PBZ_CAP_READ）。值的文本：bool 为 0 / 1，int 为十进制整数，float 为十进制小数，enum 为序号，text 为原文。
     * 线程属于 game 的模块只能在游戏线程（回调里）读，否则 PBZ_E_THREAD；any / worker 的任意线程。 */
    PbzResult (PBZ_CALL *prop_get)(PbzPlugin* self, const char* module, const char* prop, char* buffer, int32_t size, int32_t* needed);
    /* 写属性（需要 PBZ_CAP_WRITE，任意线程）。立即检查：模块和属性存在、属性可写、值合法（类型、范围、enum 的序号或名字）；
     * 通过就排队，下一帧在游戏线程上调模块的设置函数（所以紧接着 prop_get 读到的还是旧值），并写日志
     * "PLUGIN [id] prop_set 模块.属性=值"。设置函数失败（模块当时做不了）只写日志。范围外的值不会被截断，而是 PBZ_E_ARG。 */
    PbzResult (PBZ_CALL *prop_set)(PbzPlugin* self, const char* module, const char* prop, const char* value);
    /* 调用模块的动作（需要 PBZ_CAP_WRITE）。arg 是文本参数（可为 NULL）。游戏线程上立即执行，输出文本写进 buffer
     * （没有输出为空串）；动作返回失败时 PBZ_E_STATE（buffer 里是原因）。flags = PBZ_CALL_LATER 时任意线程可调：
     * 排队执行，不返回文本。 */
    PbzResult (PBZ_CALL *action_invoke)(PbzPlugin* self, const char* module, const char* action, const char* arg, int32_t flags,
                                        char* buffer, int32_t size, int32_t* needed);
    /* 订阅模块事件（需要 PBZ_CAP_READ，任意线程）。module / event 为 NULL 或 "*" 表示全部。enable = 0 取消订阅。
     * 事件到 set_callbacks 里的 on_module_event。每个插件最多 64 条订阅。 */
    PbzResult (PBZ_CALL *module_subscribe)(PbzPlugin* self, const char* module, const char* event, int32_t enable);

    /* ---- 命令总线（需要 PBZ_CAP_COMMAND）------------------------------------------------------------------------
     * 白名单里的命令，同面板的命令文件一样的写法："命令名 参数"，命令名形如 "bones.status"。不在白名单里的命令一律
     * PBZ_E_NOT_FOUND。command_list（任意线程，不需要授权）的列：命令名 | 线程(game/any/worker) | 种类
     * (diagnostic = 只输出或写日志，不改任何持久的东西；state = 改当前状态；persist = 会写 ini 或文件) | 参数说明 | 说明。 */
    PbzResult (PBZ_CALL *command_list)(char* buffer, int32_t size, int32_t* needed);
    /* 游戏线程上立即执行属于 game / any 的命令，输出写进 buffer；失败 PBZ_E_STATE（buffer 里是原因）。属于 worker
     * 线程的命令，以及 flags = PBZ_CALL_LATER 的调用（任意线程），排队执行，结果以 host / command_done 事件回来。 */
    PbzResult (PBZ_CALL *command)(PbzPlugin* self, const char* line, int32_t flags, char* buffer, int32_t size, int32_t* needed);

    /* ---- 插件 UI 页（需要 PBZ_CAP_UI）--------------------------------------------------------------------------
     * 游戏线程。注册（或同 id 再注册＝整页替换）一页；每个插件最多 8 页。页显示在面板的「派生插件」里。
     * 控件的值在页上被玩家改动后：绑定了属性的由宿主写属性，其他的走 on_ui（游戏线程，下一帧）。 */
    PbzResult (PBZ_CALL *ui_page)(PbzPlugin* self, const PbzUiPage* page);
    PbzResult (PBZ_CALL *ui_remove_page)(PbzPlugin* self, const char* page);                                  /* 游戏线程 */
    PbzResult (PBZ_CALL *ui_set_value)(PbzPlugin* self, const char* page, const char* item, double value);   /* 任意线程；更新开关、滑条、选项的当前值 */
    PbzResult (PBZ_CALL *ui_set_text)(PbzPlugin* self, const char* page, const char* item, const char* text);  /* 任意线程；更新文字、按钮上的字 */
} PbzApi;

/* api 是否有某个函数（对更新版 MOD 新增的函数用）：PBZ_API_HAS(api, get_weapon_move) */
#define PBZ_API_HAS(api, field) ((api) && (api)->size >= offsetof(PbzApi, field) + sizeof((api)->field) && (api)->field)
/* MOD 的接口修订（旧版 MOD 没有这个字段：1） */
#define PBZ_API_REVISION_OF(api) (((api) && (api)->size >= offsetof(PbzApi, api_revision) + sizeof((api)->api_revision) && (api)->api_revision) ? (api)->api_revision : 1u)

/* 入口函数的类型（MOD 用 GetProcAddress("PbzPluginMain") 取得） */
typedef PbzResult (PBZ_CALL *PbzPluginMainFn)(const PbzApi* api, PbzPlugin* self);

/* 不放在 PBZPlugins\ 里、由别的加载器载入的 DLL（例如 UE4SS 的 C++ mod）用这个把自己登记成插件：在游戏进程里
 * 找到导出了 "PbzAttachPlugin" 的模块（豪之刃的 version.dll），用 GetProcAddress 取得后调用。任意线程；入口函数在
 * 「派生插件」打开后的下一帧、在游戏线程上调用，之后和 PBZPlugins\ 里的插件完全一样。name 显示在插件列表里。 */
typedef PbzResult (PBZ_CALL *PbzAttachPluginFn)(PbzPluginMainFn entry, const wchar_t* name);

#ifdef __cplusplus
}
#endif
#endif /* PBZ_COMBO_API_H */
